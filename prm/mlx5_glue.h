/* 垫片：mlx5_prm.h 需要 rdma-core 定义的几个 WQE 段结构，
 * 直接引真头文件而不是留空。 */
#ifndef _PRM_SHIM_MLX5_GLUE_H
#define _PRM_SHIM_MLX5_GLUE_H
#include <limits.h>
#include <infiniband/verbs.h>
#include <infiniband/mlx5dv.h>
#endif
