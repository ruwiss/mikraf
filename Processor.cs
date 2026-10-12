namespace Mikraf;

/// <summary>
/// Sample-by-sample chain. Dynamics, EQ, gain and polarity follow the OBS 32.2.1
/// obs-filters math (GPL-2.0). Noise suppression calls SpeexDSP or RNNoise the
/// same way obs-filters does, in 10 ms frames, which is the delay OBS itself adds
/// for that filter.
/// </summary>
sealed class Processor
{
    const int MaxChannels = 2;

    readonly NoiseSuppress _ns = new();
    readonly NoiseGate _gate = new();
    readonly GainFilter _gain = new();
    readonly Expander _expander = new();
    readonly Expander _upward = new();
    readonly Equalizer _eq = new();
    readonly Compressor _comp = new();
    readonly Limiter _lim = new();

    readonly float[][] _planarIn = { new float[4096], new float[4096] };
    readonly float[][] _planarOut = { new float[4096], new float[4096] };
    int _rate = 48000;
    int _channels = 2;

    public int SampleRate => _rate;
    public int Channels => _channels;

    public void Reset(int sampleRate, int channels)
    {
        _rate = sampleRate;
        _channels = Math.Clamp(channels, 1, MaxChannels);
        _ns.Reset(sampleRate, _channels);
        _gate.Reset(sampleRate, _channels);
        _gain.Reset(_channels);
        _expander.Reset(sampleRate, _channels, upward: false);
        _upward.Reset(sampleRate, _channels, upward: true);
        _eq.Reset(sampleRate, _channels);
        _comp.Reset(sampleRate, _channels);
        _lim.Reset(sampleRate, _channels);
    }

    public int Process(ReadOnlySpan<float> interleaved, Span<float> output, RackSettings s)
    {
        int channels = _channels;
        int frames = interleaved.Length / channels;
        Ensure(frames);
        for (int i = 0; i < frames; i++)
        {
            for (int c = 0; c < channels; c++)
                _planarIn[c][i] = interleaved[(i * channels) + c];
        }

        int count = frames;
        var src = _planarIn;
        var dst = _planarOut;
        if (s.Bypass)
        {
            float dry = s.MonitorGain;
            int dryWritten = frames * channels;
            for (int i = 0; i < frames; i++)
            {
                for (int c = 0; c < channels; c++)
                    output[(i * channels) + c] = src[c][i] * dry;
            }
            return dryWritten;
        }
        var order = s.Chain is { Count: > 0 }
            ? s.Chain
            : new List<string>
            {
                ObsFilters.NoiseSuppress, ObsFilters.NoiseGate, ObsFilters.Invert, ObsFilters.Gain,
                ObsFilters.Expander, ObsFilters.Upward, ObsFilters.Eq, ObsFilters.Compressor, ObsFilters.Limiter,
            };
        foreach (var id in order)
        {
            switch (id)
            {
                case ObsFilters.NoiseSuppress when s.NoiseSuppress:
                    count = Run(_ns, s, src, dst, count, ref src, ref dst);
                    break;
                case ObsFilters.NoiseGate when s.NoiseGate:
                    count = Run(_gate, s, src, dst, count, ref src, ref dst);
                    break;
                case ObsFilters.Invert when s.Invert:
                    for (int c = 0; c < channels; c++)
                    {
                        var row = src[c];
                        for (int i = 0; i < count; i++)
                            row[i] = -row[i];
                    }
                    break;
                case ObsFilters.Gain when s.Gain:
                    count = Run(_gain, s, src, dst, count, ref src, ref dst);
                    break;
                case ObsFilters.Expander when s.Expander:
                    count = Run(_expander, s, src, dst, count, ref src, ref dst);
                    break;
                case ObsFilters.Upward when s.Upward:
                    count = Run(_upward, s, src, dst, count, ref src, ref dst);
                    break;
                case ObsFilters.Eq when s.Eq:
                    count = Run(_eq, s, src, dst, count, ref src, ref dst);
                    break;
                case ObsFilters.Compressor when s.Compressor:
                    count = Run(_comp, s, src, dst, count, ref src, ref dst);
                    break;
                case ObsFilters.Limiter when s.Limiter:
                    count = Run(_lim, s, src, dst, count, ref src, ref dst);
                    break;
            }
        }

        float monitor = s.MonitorGain;
        int written = count * channels;
        for (int i = 0; i < count; i++)
        {
            for (int c = 0; c < channels; c++)
                output[(i * channels) + c] = src[c][i] * monitor;
        }
        return written;
    }

    int Run(IStage stage, RackSettings s, float[][] src, float[][] dst, int count, ref float[][] cur, ref float[][] alt)
    {
        if (!stage.Enabled(s))
            return count;
        int produced = stage.Apply(s, cur, dst, count, _channels);
        if (!ReferenceEquals(stage.Output, cur))
        {
            cur = stage.Output;
            alt = ReferenceEquals(cur, src) ? dst : src;
        }
        return produced;
    }

    void Ensure(int frames)
    {
        if (_planarIn[0].Length >= frames)
            return;
        int n = Math.Max(frames, _planarIn[0].Length * 2);
        for (int c = 0; c < MaxChannels; c++)
        {
            _planarIn[c] = new float[n];
            _planarOut[c] = new float[n];
        }
        _ns.Reset(_rate, _channels);
    }

    interface IStage
    {
        bool Enabled(RackSettings s);
        float[][] Output { get; }
        int Apply(RackSettings s, float[][] src, float[][] dst, int frames, int channels);
    }

    sealed class NoiseSuppress : IStage
    {
        IntPtr[] _spx = new IntPtr[MaxChannels];
        IntPtr[] _rnn = new IntPtr[MaxChannels];
        float[][] _hold = { Array.Empty<float>(), Array.Empty<float>() };
        float[][] _ready = { new float[8192], new float[8192] };
        int[] _filled = new int[MaxChannels];
        int _rate = 48000;
        int _channels = 1;
        int _frame = 480;
        NsMethod _method = NsMethod.Speex;
        bool _open;

        public float[][] Output => _ready;

        public void Reset(int rate, int channels)
        {
            Close();
            _rate = rate;
            _channels = channels;
            _frame = Math.Max(1, rate / 100);
            _hold[0] = new float[_frame];
            _hold[1] = new float[_frame];
            Array.Clear(_filled);
        }

        public bool Enabled(RackSettings _) => NativeNs.SpeexReady;

        public int Apply(RackSettings s, float[][] src, float[][] dst, int frames, int channels)
        {
            _ = dst;
            EnsureBackend(s);
            if (!_open)
            {
                CopyThrough(src, frames);
                return frames;
            }

            int produced = 0;
            for (int i = 0; i < frames; i++)
            {
                bool full = false;
                for (int c = 0; c < channels; c++)
                {
                    _hold[c][_filled[c]] = src[c][i];
                    _filled[c]++;
                    if (_filled[c] == _frame)
                        full = true;
                }
                if (!full)
                    continue;

                for (int c = 0; c < channels; c++)
                {
                    if (s.Method == NsMethod.Rnnoise && NativeNs.RnnoiseReady && _frame == 480 && _rate == 48000)
                        NativeNs.mikraf_rnn_run(_rnn[c], _hold[c], _frame);
                    else
                        NativeNs.mikraf_spx_run(_spx[c], _hold[c], _frame, s.SuppressDb);

                    EnsureReady(produced + _frame);
                    _hold[c].AsSpan(0, _frame).CopyTo(_ready[c].AsSpan(produced));
                    _filled[c] = 0;
                }
                produced += _frame;
            }
            return produced;
        }

        void CopyThrough(float[][] src, int frames)
        {
            EnsureReady(frames);
            for (int c = 0; c < _channels; c++)
                src[c].AsSpan(0, frames).CopyTo(_ready[c]);
        }

        void EnsureBackend(RackSettings s)
        {
            bool wantRnn = s.Method == NsMethod.Rnnoise && NativeNs.RnnoiseReady && _rate == 48000 && _frame == 480;
            var method = wantRnn ? NsMethod.Rnnoise : NsMethod.Speex;
            if (_open && method == _method)
                return;
            Close();
            _method = method;
            for (int c = 0; c < _channels; c++)
            {
                _spx[c] = NativeNs.mikraf_spx_open(_frame, _rate);
                if (method == NsMethod.Rnnoise)
                    _rnn[c] = NativeNs.mikraf_rnn_open();
            }
            _open = _spx[0] != IntPtr.Zero;
            Array.Clear(_filled);
        }

        void EnsureReady(int n)
        {
            if (_ready[0].Length >= n)
                return;
            int size = Math.Max(n, _ready[0].Length * 2);
            _ready[0] = new float[size];
            _ready[1] = new float[size];
        }

        void Close()
        {
            for (int c = 0; c < MaxChannels; c++)
            {
                if (_spx[c] != IntPtr.Zero)
                {
                    NativeNs.mikraf_spx_close(_spx[c]);
                    _spx[c] = IntPtr.Zero;
                }
                if (_rnn[c] != IntPtr.Zero)
                {
                    NativeNs.mikraf_rnn_close(_rnn[c]);
                    _rnn[c] = IntPtr.Zero;
                }
            }
            _open = false;
        }
    }

    sealed class NoiseGate : IStage
    {
        int _rate = 48000;
        int _channels = 1;
        float _open;
        float _close;
        float _decay;
        float _attack = 1;
        float _release = 1;
        float _holdTime;
        float _sampleRateI;
        bool _isOpen;
        float _attenuation;
        float _level;
        float _held;
        int _attackMs = int.MinValue;
        int _holdMs = int.MinValue;
        int _releaseMs = int.MinValue;
        float _openDb = float.NaN;
        float _closeDb = float.NaN;

        public float[][] Output { get; private set; } = new float[MaxChannels][];

        public void Reset(int rate, int channels)
        {
            _rate = rate;
            _channels = channels;
            _attackMs = int.MinValue;
            _isOpen = false;
            _attenuation = 0;
            _level = 0;
            _held = 0;
        }

        public bool Enabled(RackSettings _) => true;

        public int Apply(RackSettings s, float[][] src, float[][] dst, int frames, int channels)
        {
            _ = dst;
            Output = src;
            Sync(s);
            float open = _open;
            float close = _close;
            for (int i = 0; i < frames; i++)
            {
                float cur = 0;
                for (int c = 0; c < channels; c++)
                    cur = MathF.Max(cur, MathF.Abs(src[c][i]));

                if (cur > open && !_isOpen)
                    _isOpen = true;
                if (_level < close && _isOpen)
                {
                    _held = 0;
                    _isOpen = false;
                }

                _level = MathF.Max(_level, cur) - _decay;
                if (_isOpen)
                    _attenuation = MathF.Min(1f, _attenuation + _attack);
                else
                {
                    _held += _sampleRateI;
                    if (_held > _holdTime)
                        _attenuation = MathF.Max(0f, _attenuation - _release);
                }

                for (int c = 0; c < channels; c++)
                    src[c][i] *= _attenuation;
            }
            return frames;
        }

        void Sync(RackSettings s)
        {
            if (_attackMs == s.GateAttackMs && _holdMs == s.GateHoldMs && _releaseMs == s.GateReleaseMs
                && _openDb == s.GateOpenDb && _closeDb == s.GateCloseDb)
                return;
            _attackMs = s.GateAttackMs;
            _holdMs = s.GateHoldMs;
            _releaseMs = s.GateReleaseMs;
            _openDb = s.GateOpenDb;
            _closeDb = s.GateCloseDb;
            _open = AudioMath.DbToMul(s.GateOpenDb);
            _close = AudioMath.DbToMul(s.GateCloseDb);
            _sampleRateI = 1f / _rate;
            float attackSec = Math.Max(0, s.GateAttackMs) / 1000f;
            float releaseSec = Math.Max(0, s.GateReleaseMs) / 1000f;
            _attack = attackSec <= 0 ? 1f : 1f / (attackSec * _rate);
            _release = releaseSec <= 0 ? 1f : 1f / (releaseSec * _rate);
            float minDecay = (1f / 75f) * _rate;
            _decay = (_open - _close) / minDecay;
            _holdTime = Math.Max(0, s.GateHoldMs) / 1000f;
            _isOpen = false;
            _attenuation = 0;
            _level = 0;
            _held = 0;
        }
    }

    sealed class GainFilter : IStage
    {
        public float[][] Output { get; private set; } = new float[MaxChannels][];
        public void Reset(int channels) { }
        public bool Enabled(RackSettings _) => true;
        public int Apply(RackSettings s, float[][] src, float[][] dst, int frames, int channels)
        {
            _ = dst;
            Output = src;
            float mul = AudioMath.DbToMul(s.GainDb);
            for (int c = 0; c < channels; c++)
            {
                var row = src[c];
                for (int i = 0; i < frames; i++)
                    row[i] *= mul;
            }
            return frames;
        }
    }

    sealed class Equalizer : IStage
    {
        const float LowFreq = 800f;
        const float HighFreq = 5000f;
        const float Epsilon = 1f / 4294967295f;

        struct Band
        {
            public float Lf0, Lf1, Lf2, Lf3;
            public float Hf0, Hf1, Hf2, Hf3;
            public float D1, D2, D3;
        }

        readonly Band[] _bands = new Band[MaxChannels];
        float _lf;
        float _hf;
        int _channels = 1;
        public float[][] Output { get; private set; } = new float[MaxChannels][];

        public void Reset(int rate, int channels)
        {
            _channels = channels;
            _lf = 2f * MathF.Sin(MathF.PI * LowFreq / rate);
            _hf = 2f * MathF.Sin(MathF.PI * HighFreq / rate);
            Array.Clear(_bands);
        }

        public bool Enabled(RackSettings _) => true;

        public int Apply(RackSettings s, float[][] src, float[][] dst, int frames, int channels)
        {
            _ = dst;
            Output = src;
            float low = AudioMath.DbToMul(s.EqLow);
            float mid = AudioMath.DbToMul(s.EqMid);
            float high = AudioMath.DbToMul(s.EqHigh);
            float lf = _lf;
            float hf = _hf;
            for (int c = 0; c < channels; c++)
            {
                var b = _bands[c];
                var row = src[c];
                for (int i = 0; i < frames; i++)
                {
                    float sample = row[i];
                    b.Lf0 += lf * (sample - b.Lf0) + Epsilon;
                    b.Lf1 += lf * (b.Lf0 - b.Lf1);
                    b.Lf2 += lf * (b.Lf1 - b.Lf2);
                    b.Lf3 += lf * (b.Lf2 - b.Lf3);
                    float l = b.Lf3;

                    b.Hf0 += hf * (sample - b.Hf0) + Epsilon;
                    b.Hf1 += hf * (b.Hf0 - b.Hf1);
                    b.Hf2 += hf * (b.Hf1 - b.Hf2);
                    b.Hf3 += hf * (b.Hf2 - b.Hf3);
                    float h = b.D3 - b.Hf3;
                    float m = b.D3 - (h + l);

                    b.D3 = b.D2;
                    b.D2 = b.D1;
                    b.D1 = sample;
                    row[i] = (l * low) + (m * mid) + (h * high);
                }
                _bands[c] = b;
            }
            return frames;
        }
    }

    sealed class Compressor : IStage
    {
        int _rate = 48000;
        int _channels = 1;
        float _envelope;
        float[] _env = new float[1024];
        int _attackMs = int.MinValue;
        int _releaseMs = int.MinValue;
        float _attack;
        float _release;
        public float[][] Output { get; private set; } = new float[MaxChannels][];

        public void Reset(int rate, int channels)
        {
            _rate = rate;
            _channels = channels;
            _envelope = 0;
            _attackMs = int.MinValue;
        }

        public bool Enabled(RackSettings _) => true;

        public int Apply(RackSettings s, float[][] src, float[][] dst, int frames, int channels)
        {
            _ = dst;
            Output = src;
            Sync(s);
            if (_env.Length < frames)
                _env = new float[frames];

            float attack = _attack;
            float release = _release;
            float env = _envelope;
            Array.Clear(_env, 0, frames);
            for (int c = 0; c < channels; c++)
            {
                env = _envelope;
                var row = src[c];
                for (int i = 0; i < frames; i++)
                {
                    float input = MathF.Abs(row[i]);
                    env = input + (env < input ? attack : release) * (env - input);
                    if (env > _env[i])
                        _env[i] = env;
                }
            }
            _envelope = frames > 0 ? _env[frames - 1] : _envelope;

            float slope = 1f - (1f / Math.Max(s.CompRatio, 1f));
            float threshold = s.CompThresholdDb;
            float output = AudioMath.DbToMul(s.CompOutputDb);
            for (int i = 0; i < frames; i++)
            {
                float gainDb = slope * (threshold - AudioMath.MulToDb(_env[i]));
                float gain = AudioMath.DbToMul(MathF.Min(0f, gainDb)) * output;
                for (int c = 0; c < channels; c++)
                    src[c][i] *= gain;
            }
            return frames;
        }

        void Sync(RackSettings s)
        {
            if (_attackMs == s.CompAttackMs && _releaseMs == s.CompReleaseMs)
                return;
            _attackMs = s.CompAttackMs;
            _releaseMs = s.CompReleaseMs;
            _attack = AudioMath.GainCoefficient(_rate, s.CompAttackMs / 1000f);
            _release = AudioMath.GainCoefficient(_rate, s.CompReleaseMs / 1000f);
        }
    }

    sealed class Limiter : IStage
    {
        int _rate = 48000;
        int _channels = 1;
        float _envelope;
        float[] _env = new float[1024];
        int _releaseMs = int.MinValue;
        float _attack;
        float _release;
        public float[][] Output { get; private set; } = new float[MaxChannels][];

        public void Reset(int rate, int channels)
        {
            _rate = rate;
            _channels = channels;
            _envelope = 0;
            _releaseMs = int.MinValue;
        }

        public bool Enabled(RackSettings _) => true;

        public int Apply(RackSettings s, float[][] src, float[][] dst, int frames, int channels)
        {
            _ = dst;
            Output = src;
            // OBS sets the attack argument to 0.001 and then divides by 1000 again.
            if (_releaseMs != s.LimiterReleaseMs)
            {
                _releaseMs = s.LimiterReleaseMs;
                _attack = AudioMath.GainCoefficient(_rate, 0.001f / 1000f);
                _release = AudioMath.GainCoefficient(_rate, Math.Max(1, s.LimiterReleaseMs) / 1000f);
            }
            if (_env.Length < frames)
                _env = new float[frames];

            float env = _envelope;
            Array.Clear(_env, 0, frames);
            for (int c = 0; c < channels; c++)
            {
                env = _envelope;
                var row = src[c];
                for (int i = 0; i < frames; i++)
                {
                    float input = MathF.Abs(row[i]);
                    env = input + (env < input ? _attack : _release) * (env - input);
                    if (env > _env[i])
                        _env[i] = env;
                }
            }
            _envelope = frames > 0 ? _env[frames - 1] : _envelope;

            float threshold = s.LimiterThresholdDb;
            for (int i = 0; i < frames; i++)
            {
                float gainDb = threshold - AudioMath.MulToDb(_env[i]);
                float gain = AudioMath.DbToMul(MathF.Min(0f, gainDb));
                for (int c = 0; c < channels; c++)
                    src[c][i] *= gain;
            }
            return frames;
        }
    }

    sealed class Expander : IStage
    {
        bool _upward;
        int _rate = 48000;
        int _channels = 1;
        float[] _runave = new float[MaxChannels];
        float[] _gainState = new float[MaxChannels];
        float[][] _env = { new float[1024], new float[1024] };
        float[][] _gainDb = { new float[1024], new float[1024] };
        int _attackMs = int.MinValue;
        int _releaseMs = int.MinValue;
        float _attack;
        float _release;
        public float[][] Output { get; private set; } = new float[MaxChannels][];

        public void Reset(int rate, int channels, bool upward)
        {
            _upward = upward;
            _rate = rate;
            _channels = channels;
            Array.Clear(_runave);
            Array.Clear(_gainState);
            _attackMs = int.MinValue;
        }

        public bool Enabled(RackSettings _) => true;

        public int Apply(RackSettings s, float[][] src, float[][] dst, int frames, int channels)
        {
            _ = dst;
            Output = src;
            int attackMs = _upward ? s.UpwardAttackMs : s.ExpanderAttackMs;
            int releaseMs = _upward ? s.UpwardReleaseMs : s.ExpanderReleaseMs;
            if (_attackMs != attackMs || _releaseMs != releaseMs)
            {
                _attackMs = attackMs;
                _releaseMs = releaseMs;
                _attack = AudioMath.GainCoefficient(_rate, Math.Max(1, attackMs) / 1000f);
                _release = AudioMath.GainCoefficient(_rate, Math.Max(1, releaseMs) / 1000f);
            }
            Ensure(frames);

            float ratio = _upward ? s.UpwardRatio : s.ExpanderRatio;
            float threshold = _upward ? s.UpwardThresholdDb : s.ExpanderThresholdDb;
            float output = AudioMath.DbToMul(_upward ? s.UpwardOutputDb : s.ExpanderOutputDb);
            float slope = 1f - ratio;
            float knee = _upward ? s.UpwardKneeDb : 0f;
            var detector = _upward ? s.UpwardDetector : s.ExpanderDetector;
            float rms = MathF.Pow(2f, -100f / _rate);
            float invAttack = 1f - _attack;
            float invRelease = 1f - _release;

            for (int c = 0; c < channels; c++)
            {
                var row = src[c];
                var env = _env[c];
                var run = _runave[c];
                if (detector == Detector.Rms)
                {
                    for (int i = 0; i < frames; i++)
                    {
                        float sample = row[i];
                        run = (rms * run) + ((1f - rms) * sample * sample);
                        env[i] = MathF.Sqrt(MathF.Max(run, 0f));
                    }
                }
                else
                {
                    for (int i = 0; i < frames; i++)
                        env[i] = MathF.Abs(row[i]);
                }
                _runave[c] = run;

                var gains = _gainDb[c];
                float prev = _gainState[c];
                for (int i = 0; i < frames; i++)
                {
                    float envDb = AudioMath.MulToDb(env[i]);
                    float diff = threshold - envDb;
                    if (_upward && envDb <= (threshold - 60f) / 2f)
                        diff = envDb + 60f > 0f ? envDb + 60f : 0f;

                    float target;
                    if (_upward)
                    {
                        prev = i > 0 ? MathF.Max(gains[i - 1], 0f) : MathF.Max(_gainState[c], 0f);
                        if (knee <= 0.0001f)
                            target = envDb >= threshold ? 0f : slope * diff;
                        else if (envDb >= threshold + (knee / 2f))
                            target = 0f;
                        else if (threshold - (knee / 2f) >= envDb)
                            target = slope * diff;
                        else
                            target = slope * MathF.Pow(diff + (knee / 2f), 2f) / (2f * knee);
                    }
                    else
                    {
                        prev = i > 0 ? gains[i - 1] : _gainState[c];
                        target = diff > 0f ? MathF.Max(slope * diff, -60f) : 0f;
                    }

                    gains[i] = target > prev
                        ? (_attack * prev) + (invAttack * target)
                        : (_release * prev) + (invRelease * target);

                    float linear = _upward
                        ? AudioMath.DbToMul(gains[i])
                        : AudioMath.DbToMul(MathF.Min(0f, gains[i]));
                    row[i] *= linear * output;
                }
                if (frames > 0)
                    _gainState[c] = gains[frames - 1];
            }
            return frames;
        }

        void Ensure(int frames)
        {
            if (_env[0].Length >= frames)
                return;
            _env[0] = new float[frames];
            _env[1] = new float[frames];
            _gainDb[0] = new float[frames];
            _gainDb[1] = new float[frames];
        }
    }
}
