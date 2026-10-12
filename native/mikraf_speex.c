/* SpeexDSP bridge. Noise suppression matches the OBS 32.2.1 call:
   speex_preprocess_ctl(SET_NOISE_SUPPRESS) then speex_preprocess_run
   on 16-bit samples scaled by INT16_MAX / (INT16_MAX+1). */
#include <speex/speex_preprocess.h>

/* Echo cancellation is not part of the OBS noise suppressor. The symbol is
   still referenced by speex_preprocess_run, so keep a stub that is never used. */
struct SpeexEchoState_;
void speex_echo_get_residual(struct SpeexEchoState_ *st, float *yout, int len)
{
	(void)st;
	(void)yout;
	(void)len;
}

#define FRAME_CAP 960

__declspec(dllexport) void *mikraf_spx_open(int frame, int rate)
{
	return speex_preprocess_state_init(frame, rate);
}

__declspec(dllexport) void mikraf_spx_close(void *st)
{
	if (st)
		speex_preprocess_state_destroy((SpeexPreprocessState *)st);
}

__declspec(dllexport) void mikraf_spx_run(void *st, float *samples, int n, int suppress_db)
{
	spx_int16_t tmp[FRAME_CAP];
	const float to16 = 32767.0f;
	const float from16 = 32768.0f;
	int i;

	if (!st || !samples || n <= 0)
		return;
	if (n > FRAME_CAP)
		n = FRAME_CAP;

	speex_preprocess_ctl((SpeexPreprocessState *)st, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &suppress_db);

	for (i = 0; i < n; i++) {
		float x = samples[i];
		if (x > 1.0f)
			x = 1.0f;
		else if (x < -1.0f)
			x = -1.0f;
		tmp[i] = (spx_int16_t)(x * to16);
	}

	speex_preprocess_run((SpeexPreprocessState *)st, tmp);

	for (i = 0; i < n; i++)
		samples[i] = (float)tmp[i] / from16;
}
