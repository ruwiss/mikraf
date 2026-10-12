using System.Runtime.InteropServices;

namespace Mikraf;

static class NativeNs
{
    const string SpeexDll = "mikraf-speex.dll";
    const string RnnDll = "mikraf-rnnoise.dll";

    [DllImport(SpeexDll, CallingConvention = CallingConvention.Cdecl)]
    public static extern IntPtr mikraf_spx_open(int frame, int rate);

    [DllImport(SpeexDll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void mikraf_spx_close(IntPtr st);

    [DllImport(SpeexDll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void mikraf_spx_run(IntPtr st, float[] samples, int n, int suppressDb);

    [DllImport(RnnDll, CallingConvention = CallingConvention.Cdecl)]
    public static extern IntPtr mikraf_rnn_open();

    [DllImport(RnnDll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void mikraf_rnn_close(IntPtr st);

    [DllImport(RnnDll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int mikraf_rnn_frame();

    [DllImport(RnnDll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void mikraf_rnn_run(IntPtr st, float[] samples, int n);

    public static bool SpeexReady { get; private set; }
    public static bool RnnoiseReady { get; private set; }
    public static string Status { get; private set; } = "Gürültü bastırma henüz yüklenmedi.";

    public static void Probe()
    {
        SpeexReady = false;
        RnnoiseReady = false;
        try
        {
            var st = mikraf_spx_open(480, 48000);
            if (st == IntPtr.Zero)
                throw new InvalidOperationException("speex_preprocess_state_init boş döndü.");
            var buf = new float[480];
            mikraf_spx_run(st, buf, 480, -10);
            mikraf_spx_close(st);
            SpeexReady = true;
        }
        catch (Exception ex)
        {
            Status = "Speex yüklenemedi: " + ex.Message;
            return;
        }

        try
        {
            var st = mikraf_rnn_open();
            if (st == IntPtr.Zero)
                throw new InvalidOperationException("rnnoise_create boş döndü.");
            int frame = mikraf_rnn_frame();
            var buf = new float[frame];
            mikraf_rnn_run(st, buf, frame);
            mikraf_rnn_close(st);
            RnnoiseReady = true;
            Status = "Speex ve RNNoise hazır.";
        }
        catch (Exception ex)
        {
            Status = "Speex hazır. RNNoise yüklenemedi: " + ex.Message;
        }
    }
}
