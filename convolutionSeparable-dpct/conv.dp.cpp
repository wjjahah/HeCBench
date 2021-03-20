/*
 * Copyright 1993-2010 NVIDIA Corporation.  All rights reserved.
 *
 * Please refer to the NVIDIA end user license agreement (EULA) associated
 * with this source code for terms and conditions that govern your use of
 * this software. Any use, reproduction, disclosure, or distribution of
 * this software and related documentation outside the terms of the EULA
 * is strictly prohibited.
 *
 */

#include <CL/sycl.hpp>
#include <dpct/dpct.hpp>
#include <cassert>
#include "conv.h"

#define ROWS_BLOCKDIM_X       16
#define COLUMNS_BLOCKDIM_X    16
#define ROWS_BLOCKDIM_Y       4
#define COLUMNS_BLOCKDIM_Y    8
#define ROWS_RESULT_STEPS     8
#define COLUMNS_RESULT_STEPS  8
#define ROWS_HALO_STEPS       1
#define COLUMNS_HALO_STEPS    1

void conv_rows(
    float *dst,
    const float *src,
    const float *kernel,
    const int imageW,
    const int imageH,
    const int pitch,
    sycl::nd_item<3> item_ct1,
    dpct::accessor<float, dpct::local, 2> l_Data)
{

  int gidX = item_ct1.get_group(2);
  int gidY = item_ct1.get_group(1);
  int lidX = item_ct1.get_local_id(2);
  int lidY = item_ct1.get_local_id(1);
  //Offset to the left halo edge
  const int baseX = (gidX * ROWS_RESULT_STEPS - ROWS_HALO_STEPS) * ROWS_BLOCKDIM_X + lidX;
  const int baseY = gidY * ROWS_BLOCKDIM_Y + lidY;

  src += baseY * pitch + baseX;
  dst += baseY * pitch + baseX;

  //Load main data
  for(int i = ROWS_HALO_STEPS; i < ROWS_HALO_STEPS + ROWS_RESULT_STEPS; i++)
    l_Data[lidY][lidX + i * ROWS_BLOCKDIM_X] = src[i * ROWS_BLOCKDIM_X];

  //Load left halo
  for(int i = 0; i < ROWS_HALO_STEPS; i++)
    l_Data[lidY][lidX + i * ROWS_BLOCKDIM_X]  = (baseX + i * ROWS_BLOCKDIM_X >= 0) ? src[i * ROWS_BLOCKDIM_X] : 0;

  //Load right halo
  for(int i = ROWS_HALO_STEPS + ROWS_RESULT_STEPS; i < ROWS_HALO_STEPS + ROWS_RESULT_STEPS + ROWS_HALO_STEPS; i++)
    l_Data[lidY][lidX + i * ROWS_BLOCKDIM_X]  = (baseX + i * ROWS_BLOCKDIM_X < imageW) ? src[i * ROWS_BLOCKDIM_X] : 0;

  //Compute and store results
  item_ct1.barrier();
  for(int i = ROWS_HALO_STEPS; i < ROWS_HALO_STEPS + ROWS_RESULT_STEPS; i++){
    float sum = 0;

    for(int j = -KERNEL_RADIUS; j <= KERNEL_RADIUS; j++)
      sum += kernel[KERNEL_RADIUS - j] * l_Data[lidY][lidX + i * ROWS_BLOCKDIM_X + j];

    dst[i * ROWS_BLOCKDIM_X] = sum;
  }
}

void conv_cols(
    float *dst,
    const float *src,
    const float *kernel,
    const int imageW,
    const int imageH,
    const int pitch,
    sycl::nd_item<3> item_ct1,
    dpct::accessor<float, dpct::local, 2> l_Data)
{

  int gidX = item_ct1.get_group(2);
  int gidY = item_ct1.get_group(1);
  int lidX = item_ct1.get_local_id(2);
  int lidY = item_ct1.get_local_id(1);

  //Offset to the upper halo edge
  const int baseX = gidX * COLUMNS_BLOCKDIM_X + lidX;
  const int baseY = (gidY * COLUMNS_RESULT_STEPS - COLUMNS_HALO_STEPS) * COLUMNS_BLOCKDIM_Y + lidY;
  src += baseY * pitch + baseX;
  dst += baseY * pitch + baseX;

  //Load main data
  for(int i = COLUMNS_HALO_STEPS; i < COLUMNS_HALO_STEPS + COLUMNS_RESULT_STEPS; i++)
    l_Data[lidX][lidY + i * COLUMNS_BLOCKDIM_Y] = src[i * COLUMNS_BLOCKDIM_Y * pitch];

  //Load upper halo
  for(int i = 0; i < COLUMNS_HALO_STEPS; i++)
    l_Data[lidX][lidY + i * COLUMNS_BLOCKDIM_Y] = (baseY + i * COLUMNS_BLOCKDIM_Y >= 0) ? src[i * COLUMNS_BLOCKDIM_Y * pitch] : 0;

  //Load lower halo
  for(int i = COLUMNS_HALO_STEPS + COLUMNS_RESULT_STEPS; i < COLUMNS_HALO_STEPS + COLUMNS_RESULT_STEPS + COLUMNS_HALO_STEPS; i++)
    l_Data[lidX][lidY + i * COLUMNS_BLOCKDIM_Y]  = (baseY + i * COLUMNS_BLOCKDIM_Y < imageH) ? src[i * COLUMNS_BLOCKDIM_Y * pitch] : 0;

  //Compute and store results
  item_ct1.barrier();
  for(int i = COLUMNS_HALO_STEPS; i < COLUMNS_HALO_STEPS + COLUMNS_RESULT_STEPS; i++){
    float sum = 0;

    for(int j = -KERNEL_RADIUS; j <= KERNEL_RADIUS; j++)
      sum += kernel[KERNEL_RADIUS - j] * l_Data[lidX][lidY + i * COLUMNS_BLOCKDIM_Y + j];

    dst[i * COLUMNS_BLOCKDIM_Y * pitch] = sum;
  }
}

void convolutionRows(
    float* dst,
    const float* src,
    const float* kernel,
    const unsigned int imageW,
    const unsigned int imageH,
    const unsigned int pitch)
{
  assert ( ROWS_BLOCKDIM_X * ROWS_HALO_STEPS >= KERNEL_RADIUS );
  assert ( imageW % (ROWS_RESULT_STEPS * ROWS_BLOCKDIM_X) == 0 );
  assert ( imageH % ROWS_BLOCKDIM_Y == 0 );

  sycl::range<3> block(1, ROWS_BLOCKDIM_Y, ROWS_BLOCKDIM_X);
  sycl::range<3> grid(1, imageH / ROWS_BLOCKDIM_Y,
                      imageW / ROWS_RESULT_STEPS / ROWS_BLOCKDIM_X);

  /*
  DPCT1049:0: The workgroup size passed to the SYCL kernel may exceed the limit.
  To get the device limit, query info::device::max_work_group_size. Adjust the
  workgroup size if needed.
  */
  dpct::get_default_queue().submit(
      [&](sycl::handler &cgh) {
        sycl::range<2> l_Data_range_ct1(
          4 /*ROWS_BLOCKDIM_Y*/,
          160 /*(ROWS_RESULT_STEPS + 2 * ROWS_HALO_STEPS) * ROWS_BLOCKDIM_X*/);

        sycl::accessor<float, 2, sycl::access::mode::read_write,
                       sycl::access::target::local>
            l_Data_acc_ct1(l_Data_range_ct1, cgh);

        cgh.parallel_for(sycl::nd_range<3>(grid * block, block),
                         [=](sycl::nd_item<3> item_ct1) {
                           conv_rows(dst, src, kernel, imageW, imageH, imageW,
                                     item_ct1,
                                     dpct::accessor<float, dpct::local, 2>(
                                         l_Data_acc_ct1, l_Data_range_ct1));
                         });
      });
}

void convolutionColumns(
    float* dst,
    const float* src,
    const float* kernel,
    const unsigned int imageW,
    const unsigned int imageH,
    const unsigned int pitch)
{
  assert ( COLUMNS_BLOCKDIM_Y * COLUMNS_HALO_STEPS >= KERNEL_RADIUS );
  assert ( imageW % COLUMNS_BLOCKDIM_X == 0 );
  assert ( imageH % (COLUMNS_RESULT_STEPS * COLUMNS_BLOCKDIM_Y) == 0 );

  sycl::range<3> block(1, COLUMNS_BLOCKDIM_Y, COLUMNS_BLOCKDIM_X);
  sycl::range<3> grid(1, imageH / COLUMNS_RESULT_STEPS / COLUMNS_BLOCKDIM_Y,
                      imageW / COLUMNS_BLOCKDIM_X);

  /*
  DPCT1049:1: The workgroup size passed to the SYCL kernel may exceed the limit.
  To get the device limit, query info::device::max_work_group_size. Adjust the
  workgroup size if needed.
  */
  dpct::get_default_queue().submit([&](sycl::handler &cgh) {
    sycl::range<2> l_Data_range_ct1(16/*COLUMNS_BLOCKDIM_X*/, 81/*(COLUMNS_RESULT_STEPS + 2 * COLUMNS_HALO_STEPS) * COLUMNS_BLOCKDIM_Y + 1*/);

    sycl::accessor<float, 2, sycl::access::mode::read_write,
                   sycl::access::target::local>
        l_Data_acc_ct1(l_Data_range_ct1, cgh);

    cgh.parallel_for(
        sycl::nd_range<3>(grid * block, block), [=](sycl::nd_item<3> item_ct1) {
          conv_cols(dst, src, kernel, imageW, imageH, imageW, item_ct1,
                    dpct::accessor<float, dpct::local, 2>(l_Data_acc_ct1,
                                                          l_Data_range_ct1));
        });
  });
}