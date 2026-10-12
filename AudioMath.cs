namespace Mikraf;

static class AudioMath
{
    public static float DbToMul(float db)
    {
        if (float.IsNegativeInfinity(db) || db <= -100f)
            return 0f;
        return MathF.Pow(10f, db / 20f);
    }

    public static float MulToDb(float mul)
    {
        if (mul <= 0f || float.IsNaN(mul))
            return float.NegativeInfinity;
        return 20f * MathF.Log10(mul);
    }

    public static float GainCoefficient(int sampleRate, float timeSeconds)
    {
        if (timeSeconds <= 0f || sampleRate <= 0)
            return 0f;
        return MathF.Exp(-1f / (sampleRate * timeSeconds));
    }
}
