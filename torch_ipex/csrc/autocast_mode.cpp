#include <ATen/ATen.h>
#include <ATen/NativeFunctions.h>

#include <c10/util/intrusive_ptr.h>
#include <c10/core/impl/LocalDispatchKeySet.h>
#include <torch/library.h>
#include <torch/csrc/jit/frontend/tracer.h>

#include <iostream>
#include <exception>
#include "autocast_mode.h"

namespace torch_ipex {
namespace autocast {

namespace {
using weakref_type = c10::weak_intrusive_ptr<TensorImpl, UndefinedTensorImpl>;
using val_type = std::tuple<weakref_type, Tensor>;
thread_local std::unordered_map<TensorImpl *, val_type> cached_casts;

thread_local int nesting = 0;

thread_local at::ScalarType current_target_dtype = at::kFloat;
}  // namespace

bool is_autocast_enabled() {
  return c10::impl::tls_is_dispatch_key_included(c10::DispatchKey::AutocastCPU);
}

void set_autocast_enabled(bool new_enabled) {
  c10::impl::tls_set_dispatch_key_included(c10::DispatchKey::AutocastCPU,
                                           new_enabled);
}

at::ScalarType get_autocast_dtype() {
  return current_target_dtype;
}

void set_autocast_dtype(at::ScalarType dtype) {
  current_target_dtype = dtype;
}

int autocast_increment_nesting() { return ++nesting; }

int autocast_decrement_nesting() { return --nesting; }

void clear_autocast_cache() { cached_casts.clear(); }

Tensor cpu_cached_cast(at::ScalarType to_type, const Tensor& arg) {
  if (is_eligible_cpu(arg) && (arg.scalar_type() != to_type)) {
    bool can_try_cache =
        (to_type == at::kBFloat16 && arg.scalar_type() == at::kFloat &&
        arg.requires_grad() && arg.is_leaf() && !arg.is_view() && !torch::jit::tracer::isTracing()); //Leslie Disable cache when we use the jit mode

    if (can_try_cache) {
      auto it = cached_casts.find(arg.unsafeGetTensorImpl());
      if (it != cached_casts.end()) {
        return std::get<1>(it->second);
      }
    }
    auto casted_arg = arg;
    if (to_type == at::kBFloat16) {
      //casted_arg = arg.to(at::kBFloat16).to_mkldnn();
      casted_arg = arg.to(at::kBFloat16);
      // casted_arg = arg.to_mkldnn(at::kBFloat16);
    } else if (arg.scalar_type() == at::kBFloat16) {
      //casted_arg = arg.to_dense().to(at::kFloat);
      casted_arg = arg.to(at::kFloat);
      // casted_arg = arg.to_dense(at::kFloat);
    }
    if (can_try_cache) {
      cached_casts.emplace(
          arg.unsafeGetTensorImpl(),
          val_type{weakref_type(arg.getIntrusivePtr()), casted_arg});
    }
    return casted_arg;
  } else {
    return arg;
  }
}

template<DtypeCastPolicy policy, class Redispatch, Redispatch* F, class Ret, class ArgList> struct CPU_WrapFunction_ {};

template<DtypeCastPolicy policy,
         class Registered, // The signature for which we're registering.  The dispatcher's calling code invokes our
                           // registered functions with arguments matching Registered, so we register
                           // WrapFunction_::call methods with a matching signature to properly field those arguments.
                           // guts::function_traits below extracts return_type and parameter_types from Registered,
                           // which WrapFunction_ templates above use to declare their call methods.
         class Redispatch, // The signature for the function we're redispatching to.  In most cases this is the same
                           // as Registered, but for some ops (for example, ops where we append a dtype) it's useful
                           // to redispatch to a function with a different signature.
         Redispatch* F>    // The actual function we're redispatching to.
struct CPU_WrapFunction final {
  using type = CPU_WrapFunction_<
      policy, Redispatch, F, typename guts::function_traits<Registered>::return_type,
      typename guts::function_traits<Registered>::parameter_types>;
};

// DtypeCastPolicy::user_defined_dtype
template<class Redispatch, Redispatch* F, class Ret, class... Args>
struct CPU_WrapFunction_<DtypeCastPolicy::user_defined_dtype, Redispatch, F, Ret,
                        guts::typelist::typelist<Args...>> {
  static Ret call(Args... args) {
    c10::impl::ExcludeDispatchKeyGuard no_autocastCPU(DispatchKey::AutocastCPU);
    return (*F)(cpu_cached_cast(current_target_dtype, args)...);
  }
};

// DtypeCastPolicy::fp32
template<class Redispatch, Redispatch* F, class Ret, class... Args>
struct CPU_WrapFunction_<DtypeCastPolicy::fp32, Redispatch, F, Ret,
                         guts::typelist::typelist<Args...>> {
  static Ret call(Args... args) {
    c10::impl::ExcludeDispatchKeyGuard no_autocastCPU(DispatchKey::AutocastCPU);
    return (*F)(cpu_cached_cast(at::kFloat, args)...);
  }
};

// DtypeCastPolicy::promote
template<class Redispatch, Redispatch* F, class Ret, class... Args>
struct CPU_WrapFunction_<DtypeCastPolicy::promote, Redispatch, F, Ret,
                         guts::typelist::typelist<Args...>> {
  static Ret call(Args... args) {
    c10::impl::ExcludeDispatchKeyGuard no_autocastCPU(DispatchKey::AutocastCPU);
    auto to_type = promote_type(at::kHalf, args...);
    return (*F)(cpu_cached_cast(to_type, args)...);
  }
};

#define ADD_NS(RAW_OP) at::RAW_OP

#define KERNEL_CPU(FUNC, REGISTER_NAME, SIGNATURE, PRE_DEFINED_POLICY) \
  m.impl(TORCH_SELECTIVE_NAME("aten::" REGISTER_NAME), \
    &CPU_WrapFunction<DtypeCastPolicy::PRE_DEFINED_POLICY, SIGNATURE, SIGNATURE, &FUNC>::type::call);

TORCH_LIBRARY_IMPL(_, AutocastCPU, m) {
  m.fallback(torch::CppFunction::makeFallthrough());
}

TORCH_LIBRARY_IMPL(aten, AutocastCPU, m){
  //user_defined_dtype
  KERNEL_CPU(ADD_NS(conv1d), "conv1d", Tensor (const Tensor &, const Tensor &, const c10::optional<Tensor>&, IntArrayRef, IntArrayRef, IntArrayRef, int64_t), user_defined_dtype)
  KERNEL_CPU(ADD_NS(conv2d), "conv2d", Tensor (const Tensor &, const Tensor &, const c10::optional<Tensor>&, IntArrayRef, IntArrayRef, IntArrayRef, int64_t), user_defined_dtype)
  KERNEL_CPU(ADD_NS(conv3d), "conv3d", Tensor (const Tensor &, const Tensor &, const c10::optional<Tensor>&, IntArrayRef, IntArrayRef, IntArrayRef, int64_t), user_defined_dtype)
  KERNEL_CPU(ADD_NS(_log_softmax), "_log_softmax", Tensor (const Tensor &, int64_t, bool), user_defined_dtype)
  KERNEL_CPU(ADD_NS(bmm), "bmm", Tensor (const Tensor &, const Tensor &), user_defined_dtype)
  KERNEL_CPU(ADD_NS(mm), "mm", Tensor (const Tensor &, const Tensor &), user_defined_dtype)
  KERNEL_CPU(ADD_NS(baddbmm), "baddbmm", Tensor (const Tensor &, const Tensor &, const Tensor &, const Scalar&, const Scalar&), user_defined_dtype)
  KERNEL_CPU(ADD_NS(addmm), "addmm", Tensor (const Tensor &, const Tensor &, const Tensor &, const Scalar&, const Scalar&), user_defined_dtype)
  KERNEL_CPU(ADD_NS(addbmm), "addbmm", Tensor (const Tensor &, const Tensor &, const Tensor &, const Scalar&, const Scalar&), user_defined_dtype)
  KERNEL_CPU(ADD_NS(linear), "linear", Tensor (const Tensor &, const Tensor &, const c10::optional<Tensor>&), user_defined_dtype)
  //KERNEL_CPU(ADD_NS(mul), "mul.Tensor", Tensor (const Tensor &, const Tensor &), user_defined_dtype)
  //KERNEL_CPU(ADD_NS(mul), "mul.Scalar", Tensor (const Tensor &, Scalar), user_defined_dtype)

  //fp32 cast policy
  KERNEL_CPU(ADD_NS(convolution), "convolution", Tensor (const Tensor &, const Tensor &, const c10::optional<Tensor>&, IntArrayRef, IntArrayRef, IntArrayRef, bool, IntArrayRef, int64_t), fp32)
  //KERNEL_CPU(ADD_NS(batch_norm), "batch_norm", Tensor (const Tensor &, const c10::optional<Tensor>&, const c10::optional<Tensor>&, const c10::optional<Tensor>&, const c10::optional<Tensor>&, bool, double, double, bool), fp32)
  KERNEL_CPU(ADD_NS(dropout), "dropout", Tensor (const Tensor &, double, bool), fp32)
  m.impl(TORCH_SELECTIVE_NAME("aten::native_batch_norm"),
         TORCH_FN((&CPU_WrapFunction<DtypeCastPolicy::fp32,
                                 std::tuple<Tensor,Tensor,Tensor> (const Tensor &, const c10::optional<Tensor>&, const c10::optional<Tensor>&, const c10::optional<Tensor>&, const c10::optional<Tensor>&, bool, double, double),
                                 std::tuple<Tensor,Tensor,Tensor> (const Tensor &, const c10::optional<Tensor>&, const c10::optional<Tensor>&, const c10::optional<Tensor>&, const c10::optional<Tensor>&, bool, double, double),
                                 &ADD_NS(native_batch_norm)>::type::call)));
  KERNEL_CPU(ADD_NS(max_pool2d), "max_pool2d", Tensor (const Tensor &, IntArrayRef, IntArrayRef, IntArrayRef, IntArrayRef, bool), fp32)
  m.impl(TORCH_SELECTIVE_NAME("aten::topk"),
         TORCH_FN((&CPU_WrapFunction<DtypeCastPolicy::fp32,
                                 std::tuple<Tensor,Tensor> (const Tensor &, int64_t, int64_t, bool, bool),
                                 std::tuple<Tensor,Tensor> (const Tensor &, int64_t, int64_t, bool, bool),
                                 &ADD_NS(topk)>::type::call)));
  KERNEL_CPU(ADD_NS(avg_pool2d), "avg_pool2d", Tensor (const Tensor &, IntArrayRef, IntArrayRef, IntArrayRef, bool, bool, c10::optional<int64_t>), fp32)
  KERNEL_CPU(ADD_NS(avg_pool3d), "avg_pool3d", Tensor (const Tensor &, IntArrayRef, IntArrayRef, IntArrayRef, bool, bool, c10::optional<int64_t>), fp32)
  KERNEL_CPU(ADD_NS(adaptive_avg_pool2d), "adaptive_avg_pool2d", Tensor (const Tensor &, IntArrayRef), fp32)
  KERNEL_CPU(ADD_NS(relu), "relu", Tensor (const Tensor &), fp32)
  KERNEL_CPU(ADD_NS(_softmax), "_softmax", Tensor (const Tensor &, int64_t, bool), fp32)
  KERNEL_CPU(ADD_NS(gelu), "gelu", Tensor (const Tensor &), fp32)
  m.impl(TORCH_SELECTIVE_NAME("aten::native_layer_norm"),
         TORCH_FN((&CPU_WrapFunction<DtypeCastPolicy::fp32,
                                 std::tuple<Tensor,Tensor,Tensor> (const Tensor&, IntArrayRef, const c10::optional<Tensor>&, const c10::optional<Tensor>&, double),
                                 std::tuple<Tensor,Tensor,Tensor> (const Tensor&, IntArrayRef, const c10::optional<Tensor>&, const c10::optional<Tensor>&, double),
                                 &ADD_NS(native_layer_norm)>::type::call)));
  KERNEL_CPU(ADD_NS(upsample_nearest1d), "upsample_nearest1d", Tensor (const Tensor &, IntArrayRef, c10::optional<double>), fp32)
  KERNEL_CPU(ADD_NS(upsample_nearest2d), "upsample_nearest2d", Tensor (const Tensor &, IntArrayRef, c10::optional<double>, c10::optional<double>), fp32)
  KERNEL_CPU(ADD_NS(upsample_nearest3d), "upsample_nearest3d", Tensor (const Tensor &, IntArrayRef, c10::optional<double>, c10::optional<double>, c10::optional<double>), fp32)
  KERNEL_CPU(ADD_NS(upsample_linear1d), "upsample_linear1d", Tensor (const Tensor &, IntArrayRef, bool, c10::optional<double>), fp32)
  KERNEL_CPU(ADD_NS(upsample_bilinear2d), "upsample_bilinear2d", Tensor (const Tensor &, IntArrayRef, bool, c10::optional<double>, c10::optional<double>), fp32)
  KERNEL_CPU(ADD_NS(upsample_trilinear3d), "upsample_trilinear3d", Tensor (const Tensor &, IntArrayRef, bool, c10::optional<double>, c10::optional<double>, c10::optional<double>), fp32)
  KERNEL_CPU(ADD_NS(binary_cross_entropy), "binary_cross_entropy", Tensor (const Tensor &, const Tensor &, const c10::optional<Tensor>&, int64_t), fp32)
  KERNEL_CPU(ADD_NS(binary_cross_entropy_with_logits), "binary_cross_entropy_with_logits", Tensor (const Tensor &, const Tensor &, const c10::optional<Tensor>&, const c10::optional<Tensor>&, int64_t), fp32)
  KERNEL_CPU(ADD_NS(pow), "pow.Tensor_Scalar", Tensor (const Tensor &, const Scalar&), fp32)
  KERNEL_CPU(ADD_NS(pow), "pow.Tensor_Tensor", Tensor (const Tensor &, const Tensor &), fp32)
  KERNEL_CPU(ADD_NS(pow), "pow.Scalar", Tensor (const Scalar&, const Tensor &), fp32)

  KERNEL_CPU(ADD_NS(smooth_l1_loss), "smooth_l1_loss", Tensor (const Tensor &, const Tensor &, int64_t, double), fp32)
  m.impl(TORCH_SELECTIVE_NAME("aten::sort"),
         TORCH_FN((&CPU_WrapFunction<DtypeCastPolicy::fp32,
                                 std::tuple<Tensor,Tensor> (const Tensor &, int64_t, bool),
                                 std::tuple<Tensor,Tensor> (const Tensor &, int64_t, bool),
                                 &ADD_NS(sort)>::type::call)));
  KERNEL_CPU(ADD_NS(reflection_pad1d), "reflection_pad1d", Tensor (const Tensor &, IntArrayRef), fp32)
  KERNEL_CPU(ADD_NS(std), "std", Tensor (const Tensor &, bool), fp32)
  KERNEL_CPU(ADD_NS(layer_norm), "layer_norm", Tensor (const Tensor &, IntArrayRef, const c10::optional<Tensor>&, const c10::optional<Tensor>&, double, bool), fp32)
  KERNEL_CPU(ADD_NS(bucketize), "bucketize.Tensor", Tensor (const Tensor &, const Tensor &, bool, bool), fp32)
  KERNEL_CPU(ADD_NS(bucketize), "bucketize.Scalar", Tensor (const Scalar&, const Tensor &, bool, bool), fp32)

  // promote
  KERNEL_CPU(ADD_NS(cat), "cat", Tensor (TensorList, int64_t), promote)
  KERNEL_CPU(ADD_NS(stack), "stack", Tensor (TensorList, int64_t), promote)
}

}  // namespace autocast
}  // namespace torch_ipex
