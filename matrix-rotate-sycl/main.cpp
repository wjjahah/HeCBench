#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <chrono>
#include "common.h"

void rotate_matrix_serial(float *matrix, const int n) {
  for (int layer = 0; layer < n / 2; ++layer) {
    int first = layer;
    int last = n - 1 - layer;
    for(int i = first; i < last; ++i) {
      int offset = i - first;
        float top = matrix[first*n+i]; // save top
        // left -> top
        matrix[first*n+i] = matrix[(last-offset)*n+first];

        // bottom -> left
        matrix[(last-offset)*n+first] = matrix[last*n+(last-offset)];

        // right -> bottom
        matrix[last*n+(last-offset)] = matrix[i*n+last];

        // top -> right
        matrix[i*n+last] = top; // right <- saved top
    }
  }
}

int main(int argc, char** argv) {
  if (argc != 3) {
    printf("Usage: %s <matrix size> <repeat>\n", argv[0]);
    return 1;
  }
  const int n = atoi(argv[1]);
  const int repeat = atoi(argv[2]);

  float *serial_res = (float*) aligned_alloc(1024, n*n*sizeof(float));
  float *parallel_res = (float*) aligned_alloc(1024, n*n*sizeof(float));

  for (int i = 0; i < n; i++)
    for (int j = 0; j < n; j++)
      serial_res[i*n+j] = parallel_res[i*n+j] = i*n+j;

  for (int i = 0; i < repeat; i++) {
    rotate_matrix_serial(serial_res, n);
  }

  {
#ifdef USE_GPU 
  gpu_selector dev_sel;
#else
  cpu_selector dev_sel;
#endif
  queue q(dev_sel);

  buffer<float,1> d_parallel_res(parallel_res, n*n);

  q.wait();
  auto start = std::chrono::steady_clock::now();

  for (int i = 0; i < repeat; i++) {
    q.submit([&](handler &h) {
      auto matrix = d_parallel_res.get_access<sycl_read_write>(h);
      h.parallel_for<class matrix_rotate>(nd_range<1>(range<1>((n/2+255)/256*256), range<1>(256)), [=](nd_item<1> item) {
        int layer = item.get_global_id(0); 
        if (layer < n/2) {
          int first = layer;
          int last = n - 1 - layer;
          for(int i = first; i < last; ++i) {
            int offset = i - first;

            float top = matrix[first*n+i]; // save top
            // left -> top
            matrix[first*n+i] = matrix[(last-offset)*n+first];

            // bottom -> left
            matrix[(last-offset)*n+first] = matrix[last*n+(last-offset)];

            // right -> bottom
            matrix[last*n+(last-offset)] = matrix[i*n+last];

            // top -> right
            matrix[i*n+last] = top; // right <- saved top
          }
        }
      });
    });
  }

  q.wait();
  auto end = std::chrono::steady_clock::now();
  auto time = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
  printf("Average kernel execution time: %f (s)\n", (time * 1e-9f) / repeat);

  }

  bool ok = true;
  for (int i = 0; i < n; i++) {
    for (int j = 0; j < n; j++) {
      if (serial_res[i*n+j] != parallel_res[i*n+j]) {
        ok = false;
        break;
      }
    }
  }

  printf("%s\n", ok ? "PASS" : "FAIL");

  free(serial_res);
  free(parallel_res);
  return 0;
}
