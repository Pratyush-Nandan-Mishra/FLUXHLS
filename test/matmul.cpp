// FluxHLS Stage 1 test — matrix multiply with all four pragma types
#pragma HLS INTERFACE m_axi port=A
#pragma HLS INTERFACE m_axi port=B
#pragma HLS INTERFACE m_axi port=C
#pragma HLS INTERFACE s_axilite port=return
#pragma HLS ARRAY_PARTITION variable=A type=cyclic factor=4

void matmul(float *A, float *B, float *C, int N) {
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++) {
#pragma HLS PIPELINE II=1
            float sum = 0.0f;
            for (int k = 0; k < N; k++) {
                sum += A[i * N + k] * B[k * N + j];
            }
            C[i * N + j] = sum;
        }
    }
}
