// SAMPLE 1 — Vector Addition (the "Hello World" of HLS)
//
// What HLS does here:
//   - Reads A and B from DDR memory over AXI4 bus (m_axi)
//   - Pipelines the loop so one addition starts every clock cycle (II=1)
//   - Exposes N as an AXI4-Lite control register (software can set it)
//   - Generates ap_start / ap_done / ap_idle handshake signals
//
// On an FPGA this runs in ~N clock cycles instead of N*latency cycles.

#pragma HLS INTERFACE m_axi port=A        // A is read from DDR via AXI4-Master burst transfers
#pragma HLS INTERFACE m_axi port=B        // B is read from DDR via AXI4-Master burst transfers
#pragma HLS INTERFACE m_axi port=C        // C is written back to DDR via AXI4-Master burst transfers
#pragma HLS INTERFACE s_axilite port=N    // N is exposed as a software-writable AXI4-Lite control register
#pragma HLS INTERFACE s_axilite port=return  // ap_return is placed on the AXI4-Lite control bus

void vadd(float *A, float *B, float *C, int N) {   // top-level HLS kernel: element-wise vector addition C = A + B
    for (int i = 0; i < N; i++) {                  // loop over every element of the N-element vectors
#pragma HLS PIPELINE II=1                           //** pipeline this loop: start one new addition every clock cycle
        C[i] = A[i] + B[i];                        // add A[i] and B[i] and store the result in C[i]
    }
}
