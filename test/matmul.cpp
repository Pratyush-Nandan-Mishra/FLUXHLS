// FluxHLS Stage 1 test — matrix multiply with all four pragma types
#pragma HLS INTERFACE m_axi port=A            // matrix A read from DDR via AXI4-Master
#pragma HLS INTERFACE m_axi port=B            // matrix B read from DDR via AXI4-Master
#pragma HLS INTERFACE m_axi port=C            // matrix C written to DDR via AXI4-Master
#pragma HLS INTERFACE s_axilite port=return   // ap_return on AXI4-Lite control bus
#pragma HLS ARRAY_PARTITION variable=A type=cyclic factor=4  //** partition A into 4 BRAM banks for parallel row reads

void matmul(float *A, float *B, float *C, int N) {   // matrix multiply kernel: C = A × B (N×N row-major matrices)
    for (int i = 0; i < N; i++) {                    // outer loop: iterate over rows of A and rows of C
        for (int j = 0; j < N; j++) {                // middle loop: iterate over columns of B and columns of C
#pragma HLS PIPELINE II=1                            //** pipeline the j-loop: produce one output element C[i][j] per cycle
            float sum = 0.0f;                        // accumulator for the dot product of row i of A and column j of B
            for (int k = 0; k < N; k++) {            // inner loop: accumulate over the shared dimension k
                sum += A[i * N + k] * B[k * N + j]; // multiply A's row element by B's column element and accumulate
            }
            C[i * N + j] = sum;                      // store the completed dot product into C[i][j]
        }
    }
}
