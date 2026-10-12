using System.Reflection;
using System.Runtime.InteropServices;
using NAudio.CoreAudioApi;
using NAudio.CoreAudioApi.Interfaces;
using NAudio.Wave;

namespace Mikraf;

static class WasapiPeriod
{
    public static string TryLowLatency(AudioClient client, WaveFormat format, int requestedMs)
    {
        try
        {
            var raw = typeof(AudioClient).GetField("audioClientInterface", BindingFlags.Instance | BindingFlags.NonPublic)?.GetValue(client) as IAudioClient;
            if (raw == null)
                return "fallback:arabirim yok";
            var unknown = Marshal.GetIUnknownForObject(raw);
            try
            {
                var iid = new Guid("7ED4EE07-8E67-4CD4-8C1A-2B7A5987AD42");
                int hr = Marshal.QueryInterface(unknown, in iid, out var pointer);
                if (hr != 0)
                    return "fallback:QueryInterface " + hr;
                try
                {
                    var client3 = (IAudioClient3)Marshal.GetTypedObjectForIUnknown(pointer, typeof(IAudioClient3));
                    hr = client3.GetMixFormat(out var formatPointer);
                    if (hr != 0)
                        return "fallback:GetMixFormat " + hr;
                    try
                    {
                    hr = client3.GetSharedModeEnginePeriod(formatPointer, out uint def, out uint fundamental, out uint min, out uint max);
                    if (hr != 0 || fundamental == 0)
                        return "fallback:GetSharedModeEnginePeriod " + hr;
                    uint wanted = (uint)Math.Max(1, format.SampleRate * requestedMs / 1000);
                    if (wanted < min)
                        wanted = min;
                    if (wanted > max)
                        wanted = max;
                    uint periods = Math.Max(1, (wanted + fundamental - 1) / fundamental);
                    wanted = periods * fundamental;
                    if (wanted < min)
                        wanted = ((min + fundamental - 1) / fundamental) * fundamental;
                    if (wanted > max)
                        wanted = (max / fundamental) * fundamental;
                    var session = Guid.Empty;
                    hr = client3.InitializeSharedAudioStream(AudioClientStreamFlags.EventCallback, wanted, formatPointer, ref session);
                    if (hr != 0)
                        return "fallback:InitializeSharedAudioStream " + hr;
                    return "ok:" + wanted;
                    }
                    finally
                    {
                        Marshal.FreeCoTaskMem(formatPointer);
                    }
                }
                finally
                {
                    Marshal.Release(pointer);
                }
            }
            finally
            {
                Marshal.Release(unknown);
            }
        }
        catch (Exception ex)
        {
            return "fallback:" + ex.GetType().Name + " " + ex.Message;
        }
    }
}

[ComImport]
[Guid("7ED4EE07-8E67-4CD4-8C1A-2B7A5987AD42")]
[InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IAudioClient3
{
    [PreserveSig]
    int Initialize(AudioClientShareMode shareMode, AudioClientStreamFlags streamFlags, long hnsBufferDuration, long hnsPeriodicity, [In] WaveFormat format, [In] ref Guid session);

    int GetBufferSize(out uint bufferSize);

    [return: MarshalAs(UnmanagedType.I8)]
    long GetStreamLatency();

    int GetCurrentPadding(out int currentPadding);

    [PreserveSig]
    int IsFormatSupported(AudioClientShareMode shareMode, [In] WaveFormat format, IntPtr closest);

    int GetMixFormat(out IntPtr deviceFormat);

    int GetDevicePeriod(out long defaultPeriod, out long minimumPeriod);

    int Start();

    int Stop();

    int Reset();

    int SetEventHandle(IntPtr handle);

    [PreserveSig]
    int GetService([In, MarshalAs(UnmanagedType.LPStruct)] Guid interfaceId, [Out, MarshalAs(UnmanagedType.IUnknown)] out object service);

    // IAudioClient2. These slots sit between IAudioClient and IAudioClient3.
    [PreserveSig]
    int IsOffloadCapable(int category, out int offloadCapable);

    [PreserveSig]
    int SetClientProperties(IntPtr properties);

    [PreserveSig]
    int GetBufferSizeLimits(IntPtr format, int eventDriven, out long minDuration, out long maxDuration);

    [PreserveSig]
    int GetSharedModeEnginePeriod(IntPtr format, out uint defaultPeriod, out uint fundamentalPeriod, out uint minPeriod, out uint maxPeriod);

    [PreserveSig]
    int GetCurrentSharedModeEnginePeriod(out IntPtr format, out uint currentPeriod);

    [PreserveSig]
    int InitializeSharedAudioStream(AudioClientStreamFlags flags, uint periodInFrames, IntPtr format, [In] ref Guid session);
}
