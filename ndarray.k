# DotK ndarray wrapper around modules/ndarray_module.so
# Build first: make ndarray-module

from ndarray_module import nd_array as array,
                           nd_array as arrayTyped,
                           nd_zeros as zeros,
                           nd_zeros as zerosTyped,
                           nd_ones as ones,
                           nd_ones as onesTyped,
                           nd_arange as arange,
                           nd_arange as arangeTyped,
                           nd_shape as shape,
                           nd_dtype as dtype,
                           nd_reshape as reshape,
                           nd_add as add,
                           nd_matmul as matmul,
                           nd_transpose as transpose,
                           nd_sum as sum,
                           nd_sum as sumAxis,
                           nd_get as get,
                           nd_set as set,
                           nd_to_list as toList

export array, arrayTyped, zeros, zerosTyped, ones, onesTyped, arange, arangeTyped, shape, dtype, reshape, add, matmul, transpose, sum, sumAxis, get, set, toList;