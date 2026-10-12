/* RNNoise bridge. OBS 32.2.1 scales float audio by 32768 around a 480-sample frame. */
#include "rnnoise.h"

__declspec(dllexport) void *mikraf_rnn_open(void)
{
	return rnnoise_create(NULL);
}

__declspec(dllexport) void mikraf_rnn_close(void *st)
{
	if (st)
		rnnoise_destroy((DenoiseState *)st);
}

__declspec(dllexport) int mikraf_rnn_frame(void)
{
	return 480;
}

__declspec(dllexport) void mikraf_rnn_run(void *st, float *samples, int n)
{
	int i;
	if (!st || !samples || n <= 0)
		return;
	for (i = 0; i < n; i++)
		samples[i] *= 32768.0f;
	rnnoise_process_frame((DenoiseState *)st, samples, samples);
	for (i = 0; i < n; i++)
		samples[i] /= 32768.0f;
}
