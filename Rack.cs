namespace Mikraf;

enum NsMethod
{
    Speex,
    Rnnoise,
}

enum ExpanderPreset
{
    Expander,
    Gate,
}

enum Detector
{
    Rms,
    Peak,
}

static class ObsFilters
{
    public const string NoiseSuppress = "noise_suppress_filter";
    public const string NoiseGate = "noise_gate_filter";
    public const string Invert = "invert_polarity_filter";
    public const string Gain = "gain_filter";
    public const string Expander = "expander_filter";
    public const string Upward = "upward_compressor_filter";
    public const string Eq = "basic_eq_filter";
    public const string Compressor = "compressor_filter";
    public const string Limiter = "limiter_filter";

    public static readonly (string Id, string Name)[] Catalog =
    [
        (NoiseSuppress, "Noise Suppression"),
        (NoiseGate, "Noise Gate"),
        (Invert, "Invert Polarity"),
        (Gain, "Gain"),
        (Expander, "Expander"),
        (Upward, "Upward Compressor"),
        (Eq, "3-Band Equalizer"),
        (Compressor, "Compressor"),
        (Limiter, "Limiter"),
    ];

    public static string NameOf(string id)
    {
        foreach (var item in Catalog)
        {
            if (item.Id == id)
                return item.Name;
        }
        return id;
    }
}

/// <summary>
/// OBS 32.2.1 factory values. The personal Mic/Aux chain lives only in
/// <see cref="UsePreparedChain"/> and in this machine's saved rack file.
/// </summary>
sealed class RackSettings
{
    public bool NoiseSuppress;
    public NsMethod Method = NsMethod.Rnnoise;
    public int SuppressDb = -30;

    public bool NoiseGate;
    public float GateOpenDb = -26f;
    public float GateCloseDb = -32f;
    public int GateAttackMs = 25;
    public int GateHoldMs = 200;
    public int GateReleaseMs = 150;

    public bool Invert;

    public bool Gain;
    public float GainDb;

    public bool Expander;
    public ExpanderPreset ExpanderPreset = ExpanderPreset.Expander;
    public float ExpanderRatio = 2f;
    public float ExpanderThresholdDb = -40f;
    public int ExpanderAttackMs = 10;
    public int ExpanderReleaseMs = 50;
    public float ExpanderOutputDb;
    public Detector ExpanderDetector = Detector.Rms;

    public bool Upward;
    public float UpwardRatio = 0.5f;
    public float UpwardThresholdDb = -20f;
    public int UpwardAttackMs = 10;
    public int UpwardReleaseMs = 50;
    public float UpwardOutputDb;
    public int UpwardKneeDb = 10;
    public Detector UpwardDetector = Detector.Rms;

    public bool Eq;
    public float EqLow;
    public float EqMid;
    public float EqHigh;

    public bool Compressor;
    public float CompRatio = 10f;
    public float CompThresholdDb = -18f;
    public int CompAttackMs = 6;
    public int CompReleaseMs = 60;
    public float CompOutputDb;

    public bool Limiter;
    public float LimiterThresholdDb = -6f;
    public int LimiterReleaseMs = 60;

    [System.Text.Json.Serialization.JsonIgnore]
    public bool Bypass;

    public float MonitorGain = 1f;
    public int PeriodMs = 5;
    public string? CaptureId;
    public string? RenderId;

    /// <summary>
    /// OBS filter ids in processing order. Null means the older on/off flags
    /// decide, in the fixed obs-filters order.
    /// </summary>
    public List<string>? Chain;

    public void UsePreparedChain()
    {
        NoiseSuppress = true;
        Method = NsMethod.Speex;
        SuppressDb = -10;

        NoiseGate = false;
        Invert = false;
        Gain = false;
        Upward = false;

        Expander = true;
        ExpanderPreset = ExpanderPreset.Expander;
        ExpanderRatio = 3f;
        ExpanderThresholdDb = -40f;
        ExpanderAttackMs = 1;
        ExpanderReleaseMs = 40;
        ExpanderOutputDb = 3f;
        ExpanderDetector = Detector.Rms;

        Eq = true;
        EqLow = 1f;
        EqMid = -2f;
        EqHigh = 3f;

        Compressor = true;
        CompRatio = 3f;
        CompThresholdDb = -15f;
        CompAttackMs = 1;
        CompReleaseMs = 40;
        CompOutputDb = 0f;

        Limiter = true;
        LimiterThresholdDb = -2f;
        LimiterReleaseMs = 60;

        Chain =
        [
            ObsFilters.NoiseSuppress,
            ObsFilters.Expander,
            ObsFilters.Eq,
            ObsFilters.Compressor,
            ObsFilters.Limiter,
        ];
    }

    public void EnsureChain()
    {
        if (Chain != null)
            return;
        Chain = [];
        if (NoiseSuppress)
            Chain.Add(ObsFilters.NoiseSuppress);
        if (NoiseGate)
            Chain.Add(ObsFilters.NoiseGate);
        if (Invert)
            Chain.Add(ObsFilters.Invert);
        if (Gain)
            Chain.Add(ObsFilters.Gain);
        if (Expander)
            Chain.Add(ObsFilters.Expander);
        if (Upward)
            Chain.Add(ObsFilters.Upward);
        if (Eq)
            Chain.Add(ObsFilters.Eq);
        if (Compressor)
            Chain.Add(ObsFilters.Compressor);
        if (Limiter)
            Chain.Add(ObsFilters.Limiter);
    }

    public void SyncFlags()
    {
        var chain = Chain ?? [];
        NoiseSuppress = chain.Contains(ObsFilters.NoiseSuppress);
        NoiseGate = chain.Contains(ObsFilters.NoiseGate);
        Invert = chain.Contains(ObsFilters.Invert);
        Gain = chain.Contains(ObsFilters.Gain);
        Expander = chain.Contains(ObsFilters.Expander);
        Upward = chain.Contains(ObsFilters.Upward);
        Eq = chain.Contains(ObsFilters.Eq);
        Compressor = chain.Contains(ObsFilters.Compressor);
        Limiter = chain.Contains(ObsFilters.Limiter);
    }

    public RackSettings Copy()
    {
        var copy = (RackSettings)MemberwiseClone();
        if (Chain != null)
            copy.Chain = [.. Chain];
        return copy;
    }
}
