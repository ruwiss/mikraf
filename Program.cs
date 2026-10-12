using Microsoft.Win32;

namespace Mikraf;

static class Program
{
    [STAThread]
    static void Main(string[] args)
    {
        NativeNs.Probe();
        if (args.Contains("--selftest"))
        {
            int code = SelfTest.Run();
            Environment.ExitCode = code;
            return;
        }

        if (args.Contains("--layout"))
        {
            ApplicationConfiguration.Initialize();
            using var form = new MainForm();
            form.Show();
            Application.DoEvents();
            Console.WriteLine(form.AuditLayout());
            form.Close();
            return;
        }

        if (args.Contains("--live"))
        {
            Environment.ExitCode = LiveProbe.Run();
            return;
        }

        if (args.Contains("--apo-probe"))
        {
            Environment.ExitCode = ApoProbe.Run();
            return;
        }

        bool tray = args.Contains("--tray");
        using var mutex = new Mutex(true, @"Local\Mikraf.Single", out bool created);
        if (!created)
        {
            try
            {
                using var existing = EventWaitHandle.OpenExisting(@"Local\Mikraf.Show");
                existing.Set();
            }
            catch
            {
                // The first window is already on its way out.
            }
            return;
        }
        using var show = new EventWaitHandle(false, EventResetMode.AutoReset, @"Local\Mikraf.Show");
        StartupLaunch.Touch();
        ApplicationConfiguration.Initialize();
        var window = new MainForm(tray);
        var watcher = new Thread(() =>
        {
            while (show.WaitOne())
            {
                try { window.BeginInvoke(window.ShowFromTray); }
                catch { break; }
            }
        })
        { IsBackground = true };
        watcher.Start();
        Application.Run(window);
    }
}

static class StartupLaunch
{
    const string Name = "Mikraf";
    const string KeyPath = @"Software\Microsoft\Windows\CurrentVersion\Run";

    public static bool IsEnabled()
    {
        using var key = Registry.CurrentUser.OpenSubKey(KeyPath);
        return key?.GetValue(Name) is string;
    }

    public static void Touch()
    {
        using var app = Registry.CurrentUser.CreateSubKey(@"Software\Mikraf");
        if (app?.GetValue("StartupChosen") == null || IsEnabled())
            Set(true);
    }

    public static void Set(bool enabled)
    {
        using var key = Registry.CurrentUser.CreateSubKey(KeyPath);
        using var app = Registry.CurrentUser.CreateSubKey(@"Software\Mikraf");
        app?.SetValue("StartupChosen", 1);
        if (key == null)
            return;
        if (enabled)
            key.SetValue(Name, "\"" + Exe() + "\" --tray");
        else if (key.GetValue(Name) != null)
            key.DeleteValue(Name, false);
    }

    static string Exe() => Environment.ProcessPath ?? Application.ExecutablePath;
}

static class ApoProbe
{
    public static int Run()
    {
        const string id = "{0.0.1.00000000}.{46ddf4f5-bdd7-42cb-9f86-d55ae2a56275}";
        Console.WriteLine(ApoBridge.Describe(id));
        using var enumerator = new NAudio.CoreAudioApi.MMDeviceEnumerator();
        var device = enumerator.GetDevice(id);
        NAudio.CoreAudioApi.WasapiCapture capture;
        try
        {
            capture = new NAudio.CoreAudioApi.WasapiCapture(device, true, 10);
        }
        catch (Exception ex)
        {
            Console.WriteLine("capture-failed " + ex.HResult.ToString("X8"));
            Console.WriteLine(ApoBridge.LiveText());
            return 1;
        }
        using (capture)
        {
        int blocks = 0;
        capture.DataAvailable += (_, _) => blocks++;
        try
        {
            capture.StartRecording();
            Thread.Sleep(1200);
            capture.StopRecording();
        }
        catch (Exception ex)
        {
            Console.WriteLine("capture-failed " + ex.HResult.ToString("X8"));
            Console.WriteLine(ApoBridge.LiveText());
            return 1;
        }
        Console.WriteLine($"capture-blocks {blocks} {capture.WaveFormat}");
        Console.WriteLine(ApoBridge.LiveText());
        Console.WriteLine(ApoBridge.Describe(id));
        return blocks > 0 ? 0 : 1;
        }
    }
}

static class LiveProbe
{
    public static int Run()
    {
        using var enumerator = new NAudio.CoreAudioApi.MMDeviceEnumerator();
        var cap = enumerator.GetDefaultAudioEndpoint(NAudio.CoreAudioApi.DataFlow.Capture, NAudio.CoreAudioApi.Role.Console);
        var ren = enumerator.GetDefaultAudioEndpoint(NAudio.CoreAudioApi.DataFlow.Render, NAudio.CoreAudioApi.Role.Console);
        using var engine = new Engine();
        var rack = new RackSettings { NoiseSuppress = true, Method = NsMethod.Speex, SuppressDb = -10, MonitorGain = 0f, PeriodMs = 5 };
        engine.Start(cap.ID, ren.ID, rack);
        Thread.Sleep(1200);
        Console.WriteLine(engine.Status);
        Console.WriteLine(engine.PeriodDetail);
        Console.WriteLine($"in {engine.InPeakDb:0.0} dB latency {engine.EstimatedLatencyMs} ms");
        engine.Stop();
        return engine.Status.StartsWith("Ses aygıtı", StringComparison.Ordinal) ? 1 : 0;
    }
}

static class SelfTest
{
    public static int Run()
    {
        var rack = new RackSettings
        {
            NoiseSuppress = false,
            Expander = false,
            Eq = true,
            EqLow = 0,
            EqMid = 0,
            EqHigh = 0,
            Compressor = true,
            CompRatio = 10,
            CompThresholdDb = -20,
            CompAttackMs = 1,
            CompReleaseMs = 10,
            Limiter = false,
        };
        var processor = new Processor();
        processor.Reset(48000, 1);
        var input = new float[4800];
        for (int i = 0; i < input.Length; i++)
            input[i] = 0.9f * MathF.Sin(2f * MathF.PI * 440f * i / 48000f);
        var output = new float[input.Length];
        int total = 0;
        float peak = 0;
        for (int offset = 0; offset < input.Length; offset += 480)
        {
            int n = processor.Process(input.AsSpan(offset, 480), output.AsSpan(total), rack);
            for (int i = 0; i < n; i++)
            {
                if (total + i > 2400)
                    peak = MathF.Max(peak, MathF.Abs(output[total + i]));
            }
            total += n;
        }
        Console.WriteLine($"dsp-peak {peak:0.000} samples {total}");
        Console.WriteLine(NativeNs.Status);
        if (peak > 0.5f)
        {
            Console.WriteLine("compressor did not reduce a loud sine");
            return 1;
        }

        if (NativeNs.SpeexReady)
        {
            rack.NoiseSuppress = true;
            rack.Method = NsMethod.Speex;
            rack.SuppressDb = -30;
            processor.Reset(48000, 1);
            int produced = processor.Process(input.AsSpan(0, 960), output, rack);
            Console.WriteLine($"speex-first-block {produced}");
            if (produced != 960)
            {
                Console.WriteLine("speex frame size mismatch");
                return 1;
            }
        }
        var scene = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "obs-studio", "basic", "scenes", "1.json");
        if (File.Exists(scene))
        {
            var loaded = new RackSettings();
            if (!SceneLoader.TryLoad(scene, loaded, out var captureId, out var summary))
            {
                Console.WriteLine("scene load failed");
                return 1;
            }
            Console.WriteLine(summary);
            Console.WriteLine($"scene-mic {captureId} ns {loaded.NoiseSuppress} {loaded.Method} {loaded.SuppressDb} exp {loaded.ExpanderRatio} eq {loaded.EqLow},{loaded.EqMid},{loaded.EqHigh}");
            if (!loaded.NoiseSuppress || loaded.Method != NsMethod.Speex || loaded.SuppressDb != -10 || loaded.EqMid != -2f || !loaded.Limiter
                || loaded.Chain is not { Count: 5 }
                || loaded.Chain[0] != ObsFilters.NoiseSuppress
                || loaded.Chain[1] != ObsFilters.Expander
                || loaded.Chain[2] != ObsFilters.Eq
                || loaded.Chain[3] != ObsFilters.Compressor
                || loaded.Chain[4] != ObsFilters.Limiter)
            {
                Console.WriteLine("scene chain does not match the saved Mic/Aux filters");
                return 1;
            }
        }
        int apo = ApoBridge.SelfTest();
        Console.WriteLine($"apo-selftest {apo}");
        if (apo != 0)
        {
            Console.WriteLine("apo dsp selftest failed");
            return 1;
        }
        Console.WriteLine("selftest ok");
        return 0;
    }
}
