using System.Runtime.InteropServices;
using NAudio.CoreAudioApi;
using NAudio.Wave;

namespace Mikraf;

sealed class Engine : IDisposable
{
    readonly object _gate = new();
    readonly Processor _processor = new();
    Thread? _thread;
    volatile bool _run;
    RackSettings _settings = new();
    float _inPeak;
    float _outPeak;
    string _status = "Hazır.";
    string _periodDetail = "";
    int _captureRate;
    int _renderRate;
    int _periodFrames;
    int _renderFrames;
    int _renderChannels;
    float[] _processOut = new float[4096];
    float[] _resampled = new float[4096];

    public string Status
    {
        get { lock (_gate) return _status; }
    }

    public string PeriodDetail
    {
        get { lock (_gate) return _periodDetail; }
    }

    public float InPeakDb => 20f * MathF.Log10(MathF.Max(_inPeak, 1e-7f));
    public float OutPeakDb => 20f * MathF.Log10(MathF.Max(_outPeak, 1e-7f));
    public bool Running => _run;
    public int RenderChannels => _renderChannels;

    public int EstimatedLatencyMs
    {
        get
        {
            int captureMs = _periodFrames > 0 && _captureRate > 0
                ? (int)Math.Ceiling(1000.0 * _periodFrames / _captureRate)
                : _settings.PeriodMs;
            int renderMs = _renderFrames > 0 && _renderRate > 0
                ? (int)Math.Ceiling(1000.0 * _renderFrames / _renderRate)
                : _settings.PeriodMs;
            int period = captureMs + renderMs;
            if (_settings.NoiseSuppress && NativeNs.SpeexReady)
                period += 10;
            return period;
        }
    }

    public void Publish(RackSettings settings)
    {
        var copy = settings.Copy();
        lock (_gate)
            _settings = copy;
    }

    public void Start(string captureId, string renderId, RackSettings settings)
    {
        Stop();
        Publish(settings);
        _run = true;
        _thread = new Thread(() => Run(captureId, renderId))
        {
            IsBackground = true,
            Name = "Mikraf",
            Priority = ThreadPriority.Highest,
        };
        _thread.Start();
    }

    public void Stop()
    {
        _run = false;
        _thread?.Join(1500);
        _thread = null;
        _inPeak = 0;
        _outPeak = 0;
    }

    public void Dispose() => Stop();

    void SetStatus(string text)
    {
        lock (_gate)
            _status = text;
    }

    static string PeriodKind(string code) =>
        code.StartsWith("ok:", StringComparison.Ordinal)
            ? "düşük gecikmeli paylaşılan akış"
            : "sürücünün paylaşılan tamponu";

    void Run(string captureId, string renderId)
    {
        AudioClient? capture = null;
        AudioClient? render = null;
        EventWaitHandle? capEvent = null;
        EventWaitHandle? renEvent = null;
        try
        {
            using var enumerator = new MMDeviceEnumerator();
            var capDevice = enumerator.GetDevice(captureId);
            var renDevice = enumerator.GetDevice(renderId);
            capture = capDevice.AudioClient;
            render = renDevice.AudioClient;

            var capFormat = capture.MixFormat;
            var renFormat = render.MixFormat;
            int periodMs;
            lock (_gate)
                periodMs = Math.Clamp(_settings.PeriodMs, 3, 30);
            long hns = periodMs * 10_000L;

            string capPeriod = WasapiPeriod.TryLowLatency(capture, capFormat, periodMs);
            string renPeriod = WasapiPeriod.TryLowLatency(render, renFormat, periodMs);
            if (!capPeriod.StartsWith("ok:", StringComparison.Ordinal))
                capture.Initialize(AudioClientShareMode.Shared, AudioClientStreamFlags.EventCallback, hns, 0, capFormat, Guid.Empty);
            if (!renPeriod.StartsWith("ok:", StringComparison.Ordinal))
                render.Initialize(AudioClientShareMode.Shared, AudioClientStreamFlags.EventCallback, hns, 0, renFormat, Guid.Empty);

            capEvent = new EventWaitHandle(false, EventResetMode.AutoReset);
            renEvent = new EventWaitHandle(false, EventResetMode.AutoReset);
            capture.SetEventHandle(capEvent.SafeWaitHandle.DangerousGetHandle());
            render.SetEventHandle(renEvent.SafeWaitHandle.DangerousGetHandle());

            var capClient = capture.AudioCaptureClient;
            var renClient = render.AudioRenderClient;
            int capChannels = capFormat.Channels;
            int renChannels = renFormat.Channels;
            int capRate = capFormat.SampleRate;
            int renRate = renFormat.SampleRate;
            _captureRate = capRate;
            _renderRate = renRate;
            _periodFrames = capture.BufferSize;
            _renderFrames = render.BufferSize;
            _renderChannels = renChannels;
            int procChannels = Math.Clamp(capChannels, 1, 2);
            _processor.Reset(capRate, procChannels);

            int ringCapacity = Math.Max(capRate, renRate);
            var ring = new float[ringCapacity * procChannels];
            int write = 0;
            int count = 0;
            var resampler = new LinearResampler(capRate, renRate, procChannels);
            var handles = new WaitHandle[] { capEvent, renEvent };
            int burst = capRate / 100;
            int primeFrames = Math.Max(capture.BufferSize, burst) + capture.BufferSize;
            int primeSamples = primeFrames * procChannels;
            capture.Start();
            while (_run && count < primeSamples)
            {
                capEvent.WaitOne(30);
                Drain(capClient, capFormat, procChannels, ring, ref write, ref count, resampler);
            }
            render.Start();

            double capMs = capture.BufferSize * 1000.0 / capRate;
            double renMs = render.BufferSize * 1000.0 / renRate;
            lock (_gate)
                _periodDetail = $"{capPeriod} kare {capture.BufferSize} | {renPeriod} kare {render.BufferSize}";
            string map = renChannels > 2 ? $", çıkış {renChannels} kanal, ses 1–2. kanallarda" : "";
            SetStatus($"Çalışıyor. Yakalanan tampon {capMs:0.#} ms ({PeriodKind(capPeriod)}), çıkış tamponu {renMs:0.#} ms ({PeriodKind(renPeriod)}), {capRate} Hz → {renRate} Hz{map}.");

            while (_run)
            {
                int which = WaitHandle.WaitAny(handles, 40);
                if (!_run)
                    break;

                if (which == 0 || which == WaitHandle.WaitTimeout)
                    Drain(capClient, capFormat, procChannels, ring, ref write, ref count, resampler);

                if (which == 1 || which == WaitHandle.WaitTimeout)
                    Fill(render, renClient, renFormat, procChannels, ring, ref write, ref count);
            }
        }
        catch (Exception ex)
        {
            SetStatus("Ses aygıtı açılamadı: " + ex.Message);
        }
        finally
        {
            try { capture?.Stop(); } catch { /* already stopped */ }
            try { render?.Stop(); } catch { /* already stopped */ }
            capture?.Dispose();
            render?.Dispose();
            capEvent?.Dispose();
            renEvent?.Dispose();
            if (_run)
                _run = false;
            else if (!Status.StartsWith("Ses aygıtı", StringComparison.Ordinal))
                SetStatus("Durdu.");
        }
    }

    void Drain(AudioCaptureClient client, WaveFormat format, int procChannels, float[] ring, ref int write, ref int count, LinearResampler resampler)
    {
        while (_run && client.GetNextPacketSize() > 0)
        {
            var ptr = client.GetBuffer(out int frames, out var flags);
            try
            {
                int capChannels = format.Channels;
                int samples = frames * procChannels;
                var interleaved = new float[samples];
                bool silent = (flags & AudioClientBufferFlags.Silent) != 0;
                if (!silent)
                    ReadInterleaved(ptr, frames, format, procChannels, interleaved);
                float peak = 0;
                for (int i = 0; i < interleaved.Length; i++)
                    peak = MathF.Max(peak, MathF.Abs(interleaved[i]));
                _inPeak = (_inPeak * 0.85f) + (peak * 0.15f);

                RackSettings settings;
                lock (_gate)
                    settings = _settings;
                if (_processOut.Length < samples)
                    _processOut = new float[samples];
                int produced = _processor.Process(interleaved, _processOut, settings);
                float outPeak = 0;
                for (int i = 0; i < produced; i++)
                    outPeak = MathF.Max(outPeak, MathF.Abs(_processOut[i]));
                _outPeak = (_outPeak * 0.85f) + (outPeak * 0.15f);

                int worst = produced + (procChannels * 8);
                if (_renderRate != format.SampleRate && format.SampleRate > 0)
                    worst = (int)Math.Ceiling(produced * ((double)_renderRate / format.SampleRate)) + (procChannels * 8);
                if (_resampled.Length < worst)
                    _resampled = new float[worst];
                int queued = resampler.Convert(_processOut.AsSpan(0, produced), _resampled);
                WriteRing(ring, ref write, ref count, _resampled.AsSpan(0, queued));
            }
            finally
            {
                client.ReleaseBuffer(frames);
            }
        }
    }

    void Fill(AudioClient render, AudioRenderClient client, WaveFormat format, int procChannels, float[] ring, ref int write, ref int count)
    {
        int available = render.BufferSize - render.CurrentPadding;
        if (available <= 0)
            return;
        var ptr = client.GetBuffer(available);
        try
        {
            int need = available * procChannels;
            var block = new float[need];
            int got = ReadRing(ring, ref write, ref count, block);
            if (got < need)
                Array.Clear(block, got, need - got);
            WriteInterleaved(ptr, available, format, procChannels, block);
        }
        finally
        {
            client.ReleaseBuffer(available, AudioClientBufferFlags.None);
        }
    }

    static void WriteRing(float[] ring, ref int write, ref int count, ReadOnlySpan<float> src)
    {
        if (src.Length == 0)
            return;
        for (int i = 0; i < src.Length; i++)
        {
            if (count == ring.Length)
                count--;
            ring[write] = src[i];
            write++;
            if (write == ring.Length)
                write = 0;
            count++;
        }
    }

    static int ReadRing(float[] ring, ref int write, ref int count, Span<float> dst)
    {
        int n = Math.Min(dst.Length, count);
        int read = (write - count + ring.Length) % ring.Length;
        for (int i = 0; i < n; i++)
        {
            dst[i] = ring[read];
            read++;
            if (read == ring.Length)
                read = 0;
        }
        count -= n;
        return n;
    }

    static void ReadInterleaved(IntPtr ptr, int frames, WaveFormat format, int procChannels, float[] dst)
    {
        int channels = format.Channels;
        bool ieee = IsFloat(format);
        int bytesPerSample = format.BitsPerSample / 8;
        int byteCount = frames * channels * bytesPerSample;
        var bytes = new byte[byteCount];
        Marshal.Copy(ptr, bytes, 0, byteCount);
        for (int i = 0; i < frames; i++)
        {
            for (int c = 0; c < procChannels; c++)
            {
                int source = Math.Min(c, channels - 1);
                int index = (i * channels) + source;
                float sample = ieee
                    ? BitConverter.ToSingle(bytes, index * 4)
                    : bytesPerSample switch
                    {
                        2 => BitConverter.ToInt16(bytes, index * 2) / 32768f,
                        4 => BitConverter.ToInt32(bytes, index * 4) / 2147483648f,
                        _ => 0f,
                    };
                dst[(i * procChannels) + c] = sample;
            }
        }
    }

    static void WriteInterleaved(IntPtr ptr, int frames, WaveFormat format, int procChannels, float[] src)
    {
        int channels = format.Channels;
        bool ieee = IsFloat(format);
        int bytesPerSample = format.BitsPerSample / 8;
        var bytes = new byte[frames * channels * bytesPerSample];
        for (int i = 0; i < frames; i++)
        {
            for (int c = 0; c < channels; c++)
            {
                float sample = MappedSample(src, i, c, channels, procChannels);
                int index = (i * channels) + c;
                if (ieee)
                {
                    BitConverter.TryWriteBytes(bytes.AsSpan(index * 4, 4), sample);
                }
                else if (bytesPerSample == 2)
                {
                    int v = (int)Math.Clamp(sample * 32767f, short.MinValue, short.MaxValue);
                    BitConverter.TryWriteBytes(bytes.AsSpan(index * 2, 2), (short)v);
                }
                else if (bytesPerSample == 4)
                {
                    int v = (int)Math.Clamp(sample * 2147483647f, int.MinValue, int.MaxValue);
                    BitConverter.TryWriteBytes(bytes.AsSpan(index * 4, 4), v);
                }
            }
        }
        Marshal.Copy(bytes, 0, ptr, bytes.Length);
    }

    static float MappedSample(float[] src, int frame, int channel, int outChannels, int procChannels)
    {
        int at = frame * procChannels;
        if (outChannels == 1 && procChannels > 1)
        {
            float sum = 0;
            for (int c = 0; c < procChannels; c++)
                sum += src[at + c];
            return sum / procChannels;
        }
        if (channel < procChannels)
            return src[at + channel];
        if (procChannels == 1 && channel == 1)
            return src[at];
        return 0f;
    }

    static bool IsFloat(WaveFormat format)
    {
        if (format is WaveFormatExtensible ext)
            return ext.SubFormat == new Guid("00000003-0000-0010-8000-00aa00389b71");
        return format.Encoding == WaveFormatEncoding.IeeeFloat;
    }
}

sealed class LinearResampler
{
    readonly int _inRate;
    readonly int _outRate;
    readonly int _channels;
    readonly float[] _prev;
    double _pos;

    public LinearResampler(int inRate, int outRate, int channels)
    {
        _inRate = Math.Max(1, inRate);
        _outRate = Math.Max(1, outRate);
        _channels = channels;
        _prev = new float[channels];
    }

    public int Convert(ReadOnlySpan<float> input, float[] output)
    {
        int inFrames = input.Length / _channels;
        if (inFrames <= 0)
            return 0;
        if (_inRate == _outRate)
        {
            input.CopyTo(output);
            Remember(input, inFrames);
            return input.Length;
        }

        double step = (double)_inRate / _outRate;
        int written = 0;
        while (_pos < inFrames)
        {
            int left = (int)Math.Floor(_pos);
            float frac = (float)(_pos - left);
            int right = Math.Min(left + 1, inFrames - 1);
            for (int c = 0; c < _channels; c++)
            {
                float a = Sample(input, left, c);
                float b = Sample(input, right, c);
                output[written++] = a + ((b - a) * frac);
            }
            _pos += step;
        }
        Remember(input, inFrames);
        _pos -= inFrames;
        return written;
    }

    float Sample(ReadOnlySpan<float> input, int frame, int channel)
    {
        if (frame < 0)
            return _prev[channel];
        return input[(frame * _channels) + channel];
    }

    void Remember(ReadOnlySpan<float> input, int frames)
    {
        for (int c = 0; c < _channels; c++)
            _prev[c] = input[((frames - 1) * _channels) + c];
    }
}
