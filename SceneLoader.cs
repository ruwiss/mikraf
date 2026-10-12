using System.Text.Json;

namespace Mikraf;

static class SceneLoader
{
    public static bool TryLoad(string path, RackSettings target, out string captureId, out string summary)
    {
        captureId = "";
        summary = "";
        using var doc = JsonDocument.Parse(File.ReadAllText(path));
        var root = doc.RootElement;
        if (!TryFindMic(root, out var mic))
        {
            summary = "Bu sahnede mikrofon kaynağı yok.";
            return false;
        }

        if (mic.TryGetProperty("settings", out var settings) && settings.TryGetProperty("device_id", out var id))
            captureId = id.GetString() ?? "";

        string name = mic.TryGetProperty("name", out var n) ? n.GetString() ?? "mikrofon" : "mikrofon";
        if (!mic.TryGetProperty("filters", out var filters) || filters.ValueKind != JsonValueKind.Array)
        {
            summary = name + " üzerinde filtre yok.";
            return false;
        }

        target.NoiseSuppress = false;
        target.NoiseGate = false;
        target.Invert = false;
        target.Gain = false;
        target.Expander = false;
        target.Upward = false;
        target.Eq = false;
        target.Compressor = false;
        target.Limiter = false;

        var names = new List<string>();
        target.Chain = [];
        foreach (var filter in filters.EnumerateArray())
        {
            string filterId = filter.TryGetProperty("id", out var fid) ? fid.GetString() ?? "" : "";
            bool enabled = !filter.TryGetProperty("enabled", out var en) || en.GetBoolean();
            var body = filter.TryGetProperty("settings", out var s) ? s : default;
            string label = filter.TryGetProperty("name", out var fn) ? fn.GetString() ?? filterId : filterId;
            switch (filterId)
            {
                case "noise_suppress_filter":
                    target.NoiseSuppress = enabled;
                    target.SuppressDb = (int)Num(body, "suppress_level", -30);
                    target.Method = Str(body, "method", "rnnoise") == "speex" ? NsMethod.Speex : NsMethod.Rnnoise;
                    break;
                case "noise_gate_filter":
                    target.NoiseGate = enabled;
                    target.GateOpenDb = (float)Num(body, "open_threshold", -26);
                    target.GateCloseDb = (float)Num(body, "close_threshold", -32);
                    target.GateAttackMs = (int)Num(body, "attack_time", 25);
                    target.GateHoldMs = (int)Num(body, "hold_time", 200);
                    target.GateReleaseMs = (int)Num(body, "release_time", 150);
                    break;
                case "invert_polarity_filter":
                    target.Invert = enabled;
                    break;
                case "gain_filter":
                    target.Gain = enabled;
                    target.GainDb = (float)Num(body, "db", 0);
                    break;
                case "expander_filter":
                    target.Expander = enabled;
                    target.ExpanderRatio = (float)Num(body, "ratio", 2);
                    target.ExpanderThresholdDb = (float)Num(body, "threshold", -40);
                    target.ExpanderAttackMs = (int)Num(body, "attack_time", 10);
                    target.ExpanderReleaseMs = (int)Num(body, "release_time", 50);
                    target.ExpanderOutputDb = (float)Num(body, "output_gain", 0);
                    target.ExpanderDetector = Str(body, "detector", "RMS") == "peak" ? Detector.Peak : Detector.Rms;
                    target.ExpanderPreset = Str(body, "presets", "expander") == "gate" ? ExpanderPreset.Gate : ExpanderPreset.Expander;
                    break;
                case "upward_compressor_filter":
                    target.Upward = enabled;
                    target.UpwardRatio = (float)Num(body, "ratio", 0.5);
                    target.UpwardThresholdDb = (float)Num(body, "threshold", -20);
                    target.UpwardAttackMs = (int)Num(body, "attack_time", 10);
                    target.UpwardReleaseMs = (int)Num(body, "release_time", 50);
                    target.UpwardOutputDb = (float)Num(body, "output_gain", 0);
                    target.UpwardKneeDb = (int)Num(body, "knee_width", 10);
                    target.UpwardDetector = Str(body, "detector", "RMS") == "peak" ? Detector.Peak : Detector.Rms;
                    break;
                case "basic_eq_filter":
                    target.Eq = enabled;
                    target.EqLow = (float)Num(body, "low", 0);
                    target.EqMid = (float)Num(body, "mid", 0);
                    target.EqHigh = (float)Num(body, "high", 0);
                    break;
                case "compressor_filter":
                    target.Compressor = enabled;
                    target.CompRatio = (float)Num(body, "ratio", 10);
                    target.CompThresholdDb = (float)Num(body, "threshold", -18);
                    target.CompAttackMs = (int)Num(body, "attack_time", 6);
                    target.CompReleaseMs = (int)Num(body, "release_time", 60);
                    target.CompOutputDb = (float)Num(body, "output_gain", 0);
                    break;
                case "limiter_filter":
                    target.Limiter = enabled;
                    target.LimiterThresholdDb = (float)Num(body, "threshold", -6);
                    target.LimiterReleaseMs = (int)Num(body, "release_time", 60);
                    break;
                default:
                    continue;
            }
            target.Chain.Add(filterId);
            names.Add(enabled ? label : label + " (kapalı)");
        }

        summary = name + ": " + (names.Count == 0 ? "ses filtresi yok" : string.Join(" → ", names));
        return true;
    }

    static bool TryFindMic(JsonElement root, out JsonElement mic)
    {
        if (root.TryGetProperty("AuxAudioDevice1", out mic) && IsMic(mic))
            return true;
        foreach (var prop in root.EnumerateObject())
        {
            if (IsMic(prop.Value))
            {
                mic = prop.Value;
                return true;
            }
        }
        if (root.TryGetProperty("sources", out var sources) && sources.ValueKind == JsonValueKind.Array)
        {
            foreach (var source in sources.EnumerateArray())
            {
                if (IsMic(source))
                {
                    mic = source;
                    return true;
                }
            }
        }
        mic = default;
        return false;
    }

    static bool IsMic(JsonElement item)
    {
        if (item.ValueKind != JsonValueKind.Object || !item.TryGetProperty("id", out var id))
            return false;
        string value = id.GetString() ?? "";
        return value is "wasapi_input_capture" or "coreaudio_input_capture";
    }

    static double Num(JsonElement body, string key, double fallback)
    {
        if (body.ValueKind != JsonValueKind.Object || !body.TryGetProperty(key, out var value))
            return fallback;
        return value.ValueKind switch
        {
            JsonValueKind.Number => value.GetDouble(),
            JsonValueKind.String when double.TryParse(value.GetString(), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var parsed) => parsed,
            _ => fallback,
        };
    }

    static string Str(JsonElement body, string key, string fallback)
    {
        if (body.ValueKind != JsonValueKind.Object || !body.TryGetProperty(key, out var value) || value.ValueKind != JsonValueKind.String)
            return fallback;
        return value.GetString() ?? fallback;
    }
}
