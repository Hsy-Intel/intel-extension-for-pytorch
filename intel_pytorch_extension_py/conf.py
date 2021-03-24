import os
import json
import torch
import _torch_ipex as core

class AmpConf(object):
    def __init__(self, mixed_dtype = torch.bfloat16, configure_file = None):
        self.dtype = mixed_dtype
        self.configure_file = configure_file

        if self.dtype == torch.int8:
            core.clear_indicators()
        # for int8 path, if user give a exited configure file, load it.
        if self.configure_file != None and self.dtype == torch.int8:
            if os.path.exists(self.configure_file) and os.stat(self.configure_file).st_size != 0:
                with open(self.configure_file, 'r') as f:
                    configures = json.load(f)
                    core.load_indicators_file(configures)
            else:
                assert False, 'Can not load a empty file or none existed file, plese first do calibartion step'

    # for int8 quantization, will save the date after doing calibration step.
    def save(self, configure_file):
        core.add_indicators()
        configures = core.get_int8_configures()
        with open(configure_file, 'w') as fp:
            json.dump(configures, fp, indent = 4)

    def default_recipe(self, configure_file):
        elt_wise = ['relu', 'sigmoid']
        # get default recipe,
        # q+dq+conv+q+dq+relu_ => q+dq+conv+relu
        # q+dq+op1+q+dq+q+dq+op2+q+dq => q+dq+op1+q+dq+op2+q+dq
        core.add_indicators()
        configures = core.get_int8_configures()
        nums_op = len(configures)
        for i in range(nums_op):
            current_op = configures[i]
            inputs = current_op['inputs_flow']
            op_name = current_op['name']
            for inp in inputs:
                for j in range(i):
                    # assume op only has one output
                    if configures[j]['outputs_flow'][0] == inp:
                        configures[j]['post_quantized'] = False
                        # for conv+relu, linear+relu, not have quant and dequan between them.
                        if op_name in elt_wise \
                                and (configures[j]['name'] == 'conv2d' \
                                    or configures[j]['name'] == 'conv3d' \
                                    or configures[j]['name'] == 'linear'):
                            configures[i]['pre_quantized'] = False
        with open(configure_file, 'w') as fp:
            json.dump(configures, fp, indent = 4)

