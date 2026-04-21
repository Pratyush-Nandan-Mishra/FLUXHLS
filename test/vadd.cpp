// SAMPLE 1 — Vector Addition (the "Hello World" of HLS)
//
// What HLS does here:
//   - Reads A and B from DDR memory over AXI4 bus (m_axi)
//   - Pipelines the loop so one addition starts every clock cycle (II=1)
//   - Exposes N as an AXI4-Lite control register (software can set it)
//   - Generates ap_start / ap_done / ap_idle handshake signals
//
// On an FPGA this runs in ~N clock cycles instead of N*latency cycles.

#pragma HLS INTERFACE m_axi port=A
#pragma HLS INTERFACE m_axi port=B
#pragma HLS INTERFACE m_axi port=C
#pragma HLS INTERFACE s_axilite port=N
#pragma HLS INTERFACE s_axilite port=return

void vadd(float *A, float *B, float *C, int N) {
    for (int i = 0; i < N; i++) {
#pragma HLS PIPELINE II=1
        C[i] = A[i] + B[i];
    }
}
