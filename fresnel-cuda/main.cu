#include <stdlib.h>
#include <stdio.h>
#include <chrono>
#include <cuda.h>

__host__ __device__
double Fresnel_Sine_Integral(double);

void reference (const double *__restrict input,
                      double *__restrict output, const int n)
{
  for (int i = 0; i < n; i++)
    output[i] = Fresnel_Sine_Integral(input[i]);
}

__global__ void 
kernel (const double *__restrict__ input,
              double *__restrict__ output, const int n)
{
  int i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i < n)
    output[i] = Fresnel_Sine_Integral(input[i]);
}

int main(int argc, char *argv[])
{
  if (argc != 2) {
    printf("Usage: %s <repeat>\n", argv[0]);
    return 1;
  }
  const int repeat = atoi(argv[1]);

  // range [0, 8], interval 1e-7
  const double interval = 1e-7;
  const int points = (int)(8.0 / interval);
  double *x = (double*) malloc (sizeof(double) * points);
  double *output = (double*) malloc (sizeof(double) * points);
  double *h_output = (double*) malloc (sizeof(double) * points);
  for (int i = 0; i < points; i++)
    x[i] = (double)i * interval;
	   
  double *d_x;
  cudaMalloc((void**)&d_x, sizeof(double) * points);
  double *d_output;
  cudaMalloc((void**)&d_output, sizeof(double) * points);

  dim3 grids = (points + 255)/256;
  dim3 blocks = 256;

  cudaMemcpy(d_x, x, sizeof(double) * points, cudaMemcpyHostToDevice);

  cudaDeviceSynchronize();
  auto start = std::chrono::steady_clock::now();

  for (int i = 0; i < repeat; i++)
    kernel<<<grids, blocks>>>(d_x, d_output, points);

  cudaDeviceSynchronize();
  auto end = std::chrono::steady_clock::now();
  auto time = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
  printf("Average kernel execution time %f (s)\n", (time * 1e-9f) / repeat);

  cudaMemcpy(output, d_output, sizeof(double) * points, cudaMemcpyDeviceToHost);

  // verify
  reference(x, h_output, points);
  bool ok = true;
  for (int i = 0; i < points; i++) {
    if (fabs(h_output[i] - output[i]) > 1e-6) {
      printf("%lf %lf\n", h_output[i], output[i]);
      ok = false;
      break;
    }
  }
  printf("%s\n", ok ? "PASS" : "FAIL");
  
  cudaFree(d_x);
  cudaFree(d_output);
  free(x);
  free(output);
  free(h_output);
  return 0;
}
