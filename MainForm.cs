using System.Text.Json;
using Microsoft.Win32;
using NAudio.CoreAudioApi;

namespace Mikraf;

sealed class MainForm : Form
{
    static readonly Color Bg = Color.FromArgb(36, 28, 30);
    static readonly Color PanelBg = Color.FromArgb(58, 44, 46);
    static readonly Color CardBg = Color.FromArgb(48, 36, 38);
    static readonly Color CardOff = Color.FromArgb(34, 28, 30);
    static readonly Color Ink = Color.FromArgb(250, 236, 230);
    static readonly Color Muted = Color.FromArgb(186, 156, 148);
    static readonly Color Accent = Color.FromArgb(232, 126, 104);
    static readonly Color LineCol = Color.FromArgb(92, 68, 66);
    static readonly Color Input = Color.FromArgb(28, 22, 24);

    readonly RackSettings _rack = new();
    readonly ComboBox _mics = new() { DropDownStyle = ComboBoxStyle.DropDownList, Width = 360 };
    readonly Label _apoHint = Hint("Henüz uygulanmadı. Uygula, seçili mikrofonun kendisine yazar.");
    readonly Label _status = new() { AutoSize = true, MaximumSize = new Size(360, 0) };
    readonly CheckBox _startup = new() { Text = "Windows ile açılsın", AutoSize = true, ForeColor = Ink };
    readonly CheckBox _power = new() { Text = "Efektler açık (kapatınca ses olduğu gibi geçer)", AutoSize = true, Checked = true, ForeColor = Ink, Font = new Font("Segoe UI", 10f, FontStyle.Bold) };
    readonly NotifyIcon _tray;
    readonly bool _startInTray;
    bool _quit;
    bool _trayNoted;
    bool _handleReady;
    readonly CheckBox _nsOn = ToggleBox();
    readonly CheckBox _gateOn = ToggleBox();
    readonly CheckBox _invertOn = ToggleBox();
    readonly CheckBox _gainOn = ToggleBox();
    readonly CheckBox _expOn = ToggleBox();
    readonly CheckBox _upOn = ToggleBox();
    readonly CheckBox _eqOn = ToggleBox();
    readonly CheckBox _compOn = ToggleBox();
    readonly CheckBox _limOn = ToggleBox();
    readonly ComboBox _addFilter = new() { DropDownStyle = ComboBoxStyle.DropDownList, Width = 220 };
    readonly FlowLayoutPanel _filters = new() { FlowDirection = FlowDirection.TopDown, WrapContents = false, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, Padding = new Padding(0, 4, 0, 8) };
    readonly Panel _hold = new() { Visible = false, Size = new Size(1, 1) };
    Panel _scroll = null!;
    readonly ComboBox _nsMethod = new() { DropDownStyle = ComboBoxStyle.DropDownList, Width = 160 };
    readonly NumericUpDown _nsLevel = new() { Minimum = -60, Maximum = 0, Width = 60 };
    readonly NumericUpDown _gateOpen = new() { DecimalPlaces = 0, Increment = 1, Minimum = -96, Maximum = 0, Width = 60 };
    readonly NumericUpDown _gateClose = new() { DecimalPlaces = 0, Increment = 1, Minimum = -96, Maximum = 0, Width = 60 };
    readonly NumericUpDown _gateAttack = new() { Minimum = 0, Maximum = 10000, Width = 70 };
    readonly NumericUpDown _gateHold = new() { Minimum = 0, Maximum = 10000, Width = 70 };
    readonly NumericUpDown _gateRelease = new() { Minimum = 0, Maximum = 10000, Width = 70 };
    readonly NumericUpDown _gain = new() { DecimalPlaces = 1, Increment = 0.1M, Minimum = -30, Maximum = 30, Width = 70 };
    readonly ComboBox _expPreset = new() { DropDownStyle = ComboBoxStyle.DropDownList, Width = 120 };
    readonly NumericUpDown _expRatio = new() { DecimalPlaces = 1, Increment = 0.1M, Minimum = 1, Maximum = 20, Width = 70 };
    readonly NumericUpDown _expThreshold = new() { DecimalPlaces = 1, Increment = 0.1M, Minimum = -60, Maximum = 0, Width = 70 };
    readonly NumericUpDown _expAttack = new() { Minimum = 1, Maximum = 100, Width = 60 };
    readonly NumericUpDown _expRelease = new() { Minimum = 1, Maximum = 1000, Width = 70 };
    readonly NumericUpDown _expOutput = new() { DecimalPlaces = 1, Increment = 0.1M, Minimum = -32, Maximum = 32, Width = 70 };
    readonly ComboBox _expDetector = new() { DropDownStyle = ComboBoxStyle.DropDownList, Width = 80 };
    readonly NumericUpDown _upRatio = new() { DecimalPlaces = 2, Increment = 0.05M, Minimum = 0, Maximum = 1, Width = 70 };
    readonly NumericUpDown _upThreshold = new() { DecimalPlaces = 1, Increment = 0.1M, Minimum = -60, Maximum = 0, Width = 70 };
    readonly NumericUpDown _upAttack = new() { Minimum = 1, Maximum = 100, Width = 60 };
    readonly NumericUpDown _upRelease = new() { Minimum = 1, Maximum = 1000, Width = 70 };
    readonly NumericUpDown _upOutput = new() { DecimalPlaces = 1, Increment = 0.1M, Minimum = -32, Maximum = 32, Width = 70 };
    readonly NumericUpDown _upKnee = new() { Minimum = 0, Maximum = 20, Width = 50 };
    readonly NumericUpDown _eqLow = new() { DecimalPlaces = 1, Increment = 0.1M, Minimum = -20, Maximum = 20, Width = 70 };
    readonly NumericUpDown _eqMid = new() { DecimalPlaces = 1, Increment = 0.1M, Minimum = -20, Maximum = 20, Width = 70 };
    readonly NumericUpDown _eqHigh = new() { DecimalPlaces = 1, Increment = 0.1M, Minimum = -20, Maximum = 20, Width = 70 };
    readonly NumericUpDown _compRatio = new() { DecimalPlaces = 1, Increment = 0.5M, Minimum = 1, Maximum = 32, Width = 70 };
    readonly NumericUpDown _compThreshold = new() { DecimalPlaces = 1, Increment = 0.1M, Minimum = -60, Maximum = 0, Width = 70 };
    readonly NumericUpDown _compAttack = new() { Minimum = 1, Maximum = 500, Width = 60 };
    readonly NumericUpDown _compRelease = new() { Minimum = 1, Maximum = 1000, Width = 70 };
    readonly NumericUpDown _compOutput = new() { DecimalPlaces = 1, Increment = 0.1M, Minimum = -32, Maximum = 32, Width = 70 };
    readonly NumericUpDown _limThreshold = new() { DecimalPlaces = 1, Increment = 0.1M, Minimum = -60, Maximum = 0, Width = 70 };
    readonly NumericUpDown _limRelease = new() { Minimum = 1, Maximum = 1000, Width = 70 };
    readonly System.Windows.Forms.Timer _timer = new() { Interval = 100 };
    bool _loading = true;
    int _apoTick;

    static Icon AppIcon()
    {
        try
        {
            var icon = Environment.ProcessPath is { } path ? Icon.ExtractAssociatedIcon(path) : null;
            if (icon != null)
                return icon;
        }
        catch
        {
            // Falls back to the stock icon.
        }
        return (Icon)SystemIcons.Application.Clone();
    }

    public MainForm(bool startInTray = false)
    {
        _startInTray = startInTray;
        Text = "Mikraf";
        Icon = AppIcon();
        Font = new Font("Segoe UI", 9.5f);
        BackColor = Bg;
        ForeColor = Ink;
        var area = Screen.PrimaryScreen?.WorkingArea ?? new Rectangle(0, 0, 1280, 800);
        ClientSize = new Size(420, Math.Min(680, Math.Max(620, area.Height - 80)));
        MinimumSize = new Size(400, 580);
        StartPosition = FormStartPosition.CenterScreen;

        _nsMethod.Items.AddRange(new object[] { "Speex (low CPU, low quality)", "RNNoise (good quality, more CPU)" });
        _expPreset.Items.AddRange(new object[] { "Expander", "Gate" });
        _expDetector.Items.AddRange(new object[] { "RMS", "Peak" });
        foreach (var item in ObsFilters.Catalog)
            _addFilter.Items.Add(new FilterChoice(item.Id, item.Name));

        var root = new Panel { Dock = DockStyle.Fill, Padding = new Padding(12, 10, 12, 8), BackColor = Bg };

        var refresh = SoftButton("Yenile");
        refresh.Click += (_, _) => LoadDevices();
        var apoOn = PrimaryButton("Uygula");
        var apoOff = SoftButton("Kaldır");
        apoOn.Click += (_, _) => InstallApo();
        apoOff.Click += (_, _) => RemoveApo();
        var top = new FlowLayoutPanel
        {
            Dock = DockStyle.Top,
            FlowDirection = FlowDirection.TopDown,
            WrapContents = false,
            AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink,
            BackColor = BackColor,
        };
        top.Controls.Add(new Label
        {
            Text = "Mikraf",
            AutoSize = true,
            Font = new Font("Segoe UI", 15f, FontStyle.Bold),
            ForeColor = Ink,
            Margin = new Padding(0),
        });
        top.Controls.Add(Hint("Seçtiğin mikrofonun kendi sesine efekt."));
        top.Controls.Add(_mics);
        top.Controls.Add(Line(apoOn, apoOff, refresh));
        top.Controls.Add(_apoHint);
        top.Controls.Add(_power);
        top.Controls.Add(_startup);
        _power.CheckedChanged += (_, _) => Publish();

        var add = SoftButton("Ekle");
        var prepared = SoftButton("Önerilen");
        add.Click += (_, _) => AddSelectedFilter();
        prepared.Click += (_, _) => ApplyPrepared();
        var addBar = new FlowLayoutPanel
        {
            Dock = DockStyle.Top,
            FlowDirection = FlowDirection.TopDown,
            WrapContents = false,
            AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink,
            BackColor = BackColor,
            Padding = new Padding(0, 8, 0, 4),
        };
        addBar.Controls.Add(Label("Filtre"));
        addBar.Controls.Add(_addFilter);
        addBar.Controls.Add(Line(add, prepared));

        _scroll = new Panel { Dock = DockStyle.Fill, AutoScroll = true, BackColor = BackColor };
        _filters.BackColor = BackColor;
        var stack = new FlowLayoutPanel { Dock = DockStyle.Top, FlowDirection = FlowDirection.TopDown, WrapContents = false, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, BackColor = BackColor };
        stack.Controls.Add(addBar);
        stack.Controls.Add(_filters);
        _scroll.Controls.Add(stack);
        _scroll.Resize += (_, _) => FitCardWidths();

        var load = SoftButton("OBS");
        var save = SoftButton("Kaydet");
        load.Click += (_, _) => LoadSceneInteractive();
        save.Click += (_, _) => SaveRack();
        var actions = Line(load, save);
        var bottom = new Panel { Dock = DockStyle.Bottom, Height = 72, BackColor = BackColor };
        actions.Location = new Point(0, 6);
        _status.Location = new Point(0, actions.Bottom + 4);
        bottom.Controls.Add(actions);
        bottom.Controls.Add(_status);

        root.Controls.Add(_scroll);
        root.Controls.Add(bottom);
        root.Controls.Add(top);
        Controls.Add(_hold);
        Controls.Add(root);

        _timer.Tick += (_, _) => RefreshStatus();
        _startup.CheckedChanged += (_, _) =>
        {
            if (!_loading)
                StartupLaunch.Set(_startup.Checked);
        };
        _tray = new NotifyIcon
        {
            Icon = AppIcon(),
            Visible = true,
            Text = "Mikraf",
        };
        var menu = new ContextMenuStrip();
        menu.Items.Add("Aç", null, (_, _) => ShowFromTray());
        menu.Items.Add("Çıkış", null, (_, _) =>
        {
            _quit = true;
            Close();
        });
        _tray.ContextMenuStrip = menu;
        _tray.DoubleClick += (_, _) => ShowFromTray();
        FormClosing += (_, e) =>
        {
            if (_quit || e.CloseReason is CloseReason.WindowsShutDown or CloseReason.TaskManagerClosing)
                return;
            if (!Application.MessageLoop)
                return;
            e.Cancel = true;
            HideToTray();
        };
        FormClosed += (_, _) =>
        {
            _timer.Stop();
            ReadUi();
            SaveRack();
            _tray.Visible = false;
            _tray.Dispose();
        };

        foreach (Control control in new Control[]
        {
            _mics, _nsMethod, _nsLevel,
            _gateOpen, _gateClose, _gateAttack, _gateHold, _gateRelease, _gain,
            _expPreset, _expRatio, _expThreshold, _expAttack, _expRelease, _expOutput, _expDetector,
            _upRatio, _upThreshold, _upAttack, _upRelease, _upOutput, _upKnee,
            _eqLow, _eqMid, _eqHigh,
            _compRatio, _compThreshold, _compAttack, _compRelease, _compOutput,
            _limThreshold, _limRelease,
            _nsOn, _gateOn, _invertOn, _gainOn, _expOn, _upOn, _eqOn, _compOn, _limOn,
        })
        {
            PaintControl(control);
            Hook(control);
        }
        Resize += (_, _) => FitWidths();
        _expPreset.SelectedIndexChanged += (_, _) =>
        {
            if (_loading)
                return;
            ApplyExpanderPreset();
            Publish();
        };

        LoadRack();
        PushUi();
        LoadDevices();
        FitWidths();
        _startup.Checked = StartupLaunch.IsEnabled();
        _loading = false;
        Publish();
        _status.Text = "Ayarlar seçili mikrofona yazılır. Kapatınca tepside kalır.";
        _timer.Start();
    }

    protected override void SetVisibleCore(bool value)
    {
        if (_startInTray && !_handleReady)
        {
            _handleReady = true;
            value = false;
            if (!IsHandleCreated)
                CreateHandle();
        }
        base.SetVisibleCore(value);
    }

    public void ShowFromTray()
    {
        Show();
        ShowInTaskbar = true;
        WindowState = FormWindowState.Normal;
        Activate();
    }

    public void HideToTray()
    {
        Hide();
        ShowInTaskbar = false;
        if (_trayNoted)
            return;
        _trayNoted = true;
        _tray.ShowBalloonTip(2000, "Mikraf", "Efektler mikrofonda duruyor.", ToolTipIcon.None);
    }

    static void PaintControl(Control control)
    {
        if (control is ComboBox combo)
        {
            combo.FlatStyle = FlatStyle.Flat;
            combo.BackColor = Input;
            combo.ForeColor = Ink;
        }
        else if (control is NumericUpDown number)
        {
            number.BackColor = Input;
            number.ForeColor = Ink;
        }
    }

    void Hook(Control control)
    {
        switch (control)
        {
            case NumericUpDown number:
                number.ValueChanged += (_, _) => Publish();
                break;
            case ComboBox combo:
                combo.SelectedIndexChanged += (_, _) => Publish();
                break;
            case CheckBox check:
                check.CheckedChanged += (_, _) => Publish();
                break;
        }
    }

    void Publish()
    {
        if (_loading)
            return;
        ReadUi();
        _nsLevel.Enabled = _nsOn.Checked && _rack.Method != NsMethod.Rnnoise;
        _rack.Bypass = !_power.Checked;
        ApoBridge.Push(_rack);
        RefreshToggles();
    }

    void RefreshToggles()
    {
        foreach (Control card in _filters.Controls)
        {
            if (card.Tag is not string id || card.Controls.Count < 2)
                continue;
            bool on = ToggleFor(id).Checked;
            var tint = on ? CardBg : CardOff;
            card.BackColor = tint;
            foreach (Control child in card.Controls)
                child.BackColor = child is Button && child != card.Controls[1] ? PanelBg : tint;
            ToggleFor(id).BackColor = tint;
            if (card.Controls.Count > 1)
                card.Controls[1].ForeColor = on ? Ink : Muted;
        }
    }

    void ApplyExpanderPreset()
    {
        bool gate = _expPreset.SelectedIndex == 1;
        _loading = true;
        _expRatio.Value = gate ? 10 : 2;
        _expThreshold.Value = -40;
        _expAttack.Value = 10;
        _expRelease.Value = gate ? 125 : 50;
        _expOutput.Value = 0;
        _expDetector.SelectedIndex = 0;
        _loading = false;
    }

    void InstallApo()
    {
        if (_mics.SelectedItem is not DeviceItem mic)
        {
            _status.Text = "Önce mikrofon seç.";
            return;
        }
        bool voice = ApoBridge.VoiceClarityPresent(mic.Id);
        string ask = voice
            ? "Windows Ses Netliği bu efektin önüne geçiyor. Uygula onu tüm mikrofonlardan kaldırır ve geri getirmez. Seçili mikrofonun kendisine efekt yazılır. Ses birkaç saniye kesilir, sonra yeniden başlar."
            : "Seçili mikrofonun kendisine efekt uygulanır. Windows sesi birkaç saniye yeniden başlar.";
        var answer = MessageBox.Show(this, ask, "Mikraf", MessageBoxButtons.YesNo, MessageBoxIcon.Information);
        if (answer != DialogResult.Yes)
            return;
        _status.Text = "Yönetici izni bekleniyor.";
        string result = ApoBridge.Install(mic.Id);
        ApoBridge.Push(_rack);
        _apoHint.Text = ApoBridge.Describe(mic.Id);
        _status.Text = result;
    }

    void RemoveApo()
    {
        string ask = ApoBridge.VoiceClarityWasRemoved()
            ? "Mikraf mikrofonlardan kalkar. Windows Ses Netliği, kaldırıldıysa geri gelmez. Ses birkaç saniye yeniden başlar."
            : "Mikraf mikrofonlardan kalkar. Ses birkaç saniye yeniden başlar.";
        var answer = MessageBox.Show(this, ask, "Mikraf", MessageBoxButtons.YesNo, MessageBoxIcon.Information);
        if (answer != DialogResult.Yes)
            return;
        _status.Text = "Kaldırma için yönetici izni bekleniyor.";
        string result = ApoBridge.Remove();
        _apoHint.Text = ApoBridge.Describe((_mics.SelectedItem as DeviceItem)?.Id);
        _status.Text = result;
    }

    void RefreshStatus()
    {
        if (++_apoTick % 10 == 0)
            _apoHint.Text = ApoBridge.Describe((_mics.SelectedItem as DeviceItem)?.Id);
    }

    void LoadDevices()
    {
        string? micId = (_mics.SelectedItem as DeviceItem)?.Id ?? _rack.CaptureId;
        _mics.Items.Clear();
        using var enumerator = new MMDeviceEnumerator();
        foreach (var device in enumerator.EnumerateAudioEndPoints(DataFlow.Capture, DeviceState.Active))
            _mics.Items.Add(new DeviceItem(device.ID, device.FriendlyName));
        Select(_mics, micId);
    }

    static void Select(ComboBox box, string? id)
    {
        if (box.Items.Count == 0)
            return;
        for (int i = 0; i < box.Items.Count; i++)
        {
            if (box.Items[i] is DeviceItem item && item.Id == id)
            {
                box.SelectedIndex = i;
                return;
            }
        }
        box.SelectedIndex = 0;
    }

    void LoadSceneInteractive()
    {
        using var dialog = new OpenFileDialog
        {
            Filter = "OBS sahnesi|*.json",
            InitialDirectory = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "obs-studio", "basic", "scenes"),
        };
        if (dialog.ShowDialog(this) != DialogResult.OK)
            return;
        try
        {
            if (!SceneLoader.TryLoad(dialog.FileName, _rack, out var captureId, out var summary))
            {
                _status.Text = summary;
                return;
            }
            if (!string.IsNullOrWhiteSpace(captureId))
                _rack.CaptureId = captureId;
            PushUi();
            LoadDevices();
            Publish();
            _status.Text = summary;
        }
        catch (Exception ex)
        {
            _status.Text = "Sahne okunamadı: " + ex.Message;
        }
    }

    void PushUi()
    {
        _loading = true;
        _rack.EnsureChain();
        _nsMethod.SelectedIndex = _rack.Method == NsMethod.Speex ? 0 : 1;
        _nsLevel.Value = _rack.SuppressDb;
        _gateOpen.Value = (decimal)_rack.GateOpenDb;
        _gateClose.Value = (decimal)_rack.GateCloseDb;
        _gateAttack.Value = _rack.GateAttackMs;
        _gateHold.Value = _rack.GateHoldMs;
        _gateRelease.Value = _rack.GateReleaseMs;
        _gain.Value = (decimal)_rack.GainDb;
        _expPreset.SelectedIndex = _rack.ExpanderPreset == ExpanderPreset.Gate ? 1 : 0;
        _expRatio.Value = (decimal)_rack.ExpanderRatio;
        _expThreshold.Value = (decimal)_rack.ExpanderThresholdDb;
        _expAttack.Value = _rack.ExpanderAttackMs;
        _expRelease.Value = _rack.ExpanderReleaseMs;
        _expOutput.Value = (decimal)_rack.ExpanderOutputDb;
        _expDetector.SelectedIndex = _rack.ExpanderDetector == Detector.Peak ? 1 : 0;
        _upRatio.Value = (decimal)_rack.UpwardRatio;
        _upThreshold.Value = (decimal)_rack.UpwardThresholdDb;
        _upAttack.Value = _rack.UpwardAttackMs;
        _upRelease.Value = _rack.UpwardReleaseMs;
        _upOutput.Value = (decimal)_rack.UpwardOutputDb;
        _upKnee.Value = _rack.UpwardKneeDb;
        _eqLow.Value = (decimal)_rack.EqLow;
        _eqMid.Value = (decimal)_rack.EqMid;
        _eqHigh.Value = (decimal)_rack.EqHigh;
        _compRatio.Value = (decimal)_rack.CompRatio;
        _compThreshold.Value = (decimal)_rack.CompThresholdDb;
        _compAttack.Value = _rack.CompAttackMs;
        _compRelease.Value = _rack.CompReleaseMs;
        _compOutput.Value = (decimal)_rack.CompOutputDb;
        _limThreshold.Value = (decimal)_rack.LimiterThresholdDb;
        _limRelease.Value = _rack.LimiterReleaseMs;
        _nsOn.Checked = _rack.NoiseSuppress;
        _gateOn.Checked = _rack.NoiseGate;
        _invertOn.Checked = _rack.Invert;
        _gainOn.Checked = _rack.Gain;
        _expOn.Checked = _rack.Expander;
        _upOn.Checked = _rack.Upward;
        _eqOn.Checked = _rack.Eq;
        _compOn.Checked = _rack.Compressor;
        _limOn.Checked = _rack.Limiter;
        RebuildFilters();
        _loading = false;
    }

    void ReadUi()
    {
        _rack.EnsureChain();
        _rack.Method = _nsMethod.SelectedIndex == 1 ? NsMethod.Rnnoise : NsMethod.Speex;
        _rack.SuppressDb = (int)_nsLevel.Value;
        _rack.GateOpenDb = (float)_gateOpen.Value;
        _rack.GateCloseDb = (float)_gateClose.Value;
        _rack.GateAttackMs = (int)_gateAttack.Value;
        _rack.GateHoldMs = (int)_gateHold.Value;
        _rack.GateReleaseMs = (int)_gateRelease.Value;
        _rack.GainDb = (float)_gain.Value;
        _rack.ExpanderPreset = _expPreset.SelectedIndex == 1 ? ExpanderPreset.Gate : ExpanderPreset.Expander;
        _rack.ExpanderRatio = (float)_expRatio.Value;
        _rack.ExpanderThresholdDb = (float)_expThreshold.Value;
        _rack.ExpanderAttackMs = (int)_expAttack.Value;
        _rack.ExpanderReleaseMs = (int)_expRelease.Value;
        _rack.ExpanderOutputDb = (float)_expOutput.Value;
        _rack.ExpanderDetector = _expDetector.SelectedIndex == 1 ? Detector.Peak : Detector.Rms;
        _rack.UpwardRatio = (float)_upRatio.Value;
        _rack.UpwardThresholdDb = (float)_upThreshold.Value;
        _rack.UpwardAttackMs = (int)_upAttack.Value;
        _rack.UpwardReleaseMs = (int)_upRelease.Value;
        _rack.UpwardOutputDb = (float)_upOutput.Value;
        _rack.UpwardKneeDb = (int)_upKnee.Value;
        _rack.EqLow = (float)_eqLow.Value;
        _rack.EqMid = (float)_eqMid.Value;
        _rack.EqHigh = (float)_eqHigh.Value;
        _rack.CompRatio = (float)_compRatio.Value;
        _rack.CompThresholdDb = (float)_compThreshold.Value;
        _rack.CompAttackMs = (int)_compAttack.Value;
        _rack.CompReleaseMs = (int)_compRelease.Value;
        _rack.CompOutputDb = (float)_compOutput.Value;
        _rack.LimiterThresholdDb = (float)_limThreshold.Value;
        _rack.LimiterReleaseMs = (int)_limRelease.Value;
        _rack.NoiseSuppress = _nsOn.Checked;
        _rack.NoiseGate = _gateOn.Checked;
        _rack.Invert = _invertOn.Checked;
        _rack.Gain = _gainOn.Checked;
        _rack.Expander = _expOn.Checked;
        _rack.Upward = _upOn.Checked;
        _rack.Eq = _eqOn.Checked;
        _rack.Compressor = _compOn.Checked;
        _rack.Limiter = _limOn.Checked;
        if (_mics.SelectedItem is DeviceItem mic)
            _rack.CaptureId = mic.Id;
    }

    void AddSelectedFilter()
    {
        if (_addFilter.SelectedItem is not FilterChoice choice)
            return;
        ReadUi();
        if (_rack.Chain!.Contains(choice.Id))
            return;
        _rack.Chain.Add(choice.Id);
        SetFlag(choice.Id, true);
        _loading = true;
        ToggleFor(choice.Id).Checked = true;
        _loading = false;
        RebuildFilters();
        Publish();
    }

    void ApplyPrepared()
    {
        ReadUi();
        string? capture = _rack.CaptureId;
        string? render = _rack.RenderId;
        int period = _rack.PeriodMs;
        float monitor = _rack.MonitorGain;
        _rack.UsePreparedChain();
        _rack.CaptureId = capture;
        _rack.RenderId = render;
        _rack.PeriodMs = period;
        _rack.MonitorGain = monitor;
        PushUi();
        Publish();
        _status.Text = "Önerilen zincir yazıldı.";
    }

    void MoveFilter(string id, int delta)
    {
        ReadUi();
        var chain = _rack.Chain!;
        int index = chain.IndexOf(id);
        int next = index + delta;
        if (index < 0 || next < 0 || next >= chain.Count)
            return;
        (chain[index], chain[next]) = (chain[next], chain[index]);
        RebuildFilters();
        Publish();
    }

    void RemoveFilter(string id)
    {
        ReadUi();
        _rack.Chain!.Remove(id);
        SetFlag(id, false);
        _loading = true;
        ToggleFor(id).Checked = false;
        _loading = false;
        RebuildFilters();
        Publish();
    }

    void RebuildFilters()
    {
        _filters.SuspendLayout();
        ParkInputs(true);
        var stale = new Control[_filters.Controls.Count];
        _filters.Controls.CopyTo(stale, 0);
        foreach (var control in stale)
            control.Parent = null;
        var chain = _rack.Chain ?? [];
        for (int i = 0; i < chain.Count; i++)
            _filters.Controls.Add(Card(chain[i], i, chain.Count));
        foreach (var control in stale)
            control.Dispose();
        FitCardWidths();
        _addFilter.Items.Clear();
        foreach (var item in ObsFilters.Catalog)
        {
            if (!chain.Contains(item.Id))
                _addFilter.Items.Add(new FilterChoice(item.Id, item.Name));
        }
        if (_addFilter.Items.Count > 0)
            _addFilter.SelectedIndex = 0;
        _filters.ResumeLayout();
    }

    // Editor fields go back to the hold panel after a dialog; the on/off boxes stay on their cards unless the cards are rebuilt.
    void ParkInputs(bool includeToggles = false)
    {
        foreach (Control control in new Control[]
        {
            _nsMethod, _nsLevel,
            _gateOpen, _gateClose, _gateAttack, _gateHold, _gateRelease, _gain,
            _expPreset, _expRatio, _expThreshold, _expAttack, _expRelease, _expOutput, _expDetector,
            _upRatio, _upThreshold, _upAttack, _upRelease, _upOutput, _upKnee,
            _eqLow, _eqMid, _eqHigh,
            _compRatio, _compThreshold, _compAttack, _compRelease, _compOutput,
            _limThreshold, _limRelease,
        })
            control.Parent = _hold;
        if (!includeToggles)
            return;
        foreach (var toggle in new[] { _nsOn, _gateOn, _invertOn, _gainOn, _expOn, _upOn, _eqOn, _compOn, _limOn })
            toggle.Parent = _hold;
    }

    int CardWidth()
    {
        int width = _scroll.ClientSize.Width - 8;
        if (width < 360)
            width = 360;
        return width;
    }

    void FitWidths()
    {
        int inner = Math.Max(280, ClientSize.Width - 48);
        _mics.Width = inner;
        _addFilter.Width = Math.Min(210, inner);
        _status.MaximumSize = new Size(inner, 0);
        _apoHint.MaximumSize = new Size(inner, 0);
        FitCardWidths();
    }

    void FitCardWidths()
    {
        int width = CardWidth();
        foreach (Control card in _filters.Controls)
        {
            card.Width = width;
            LayoutRow(card, width);
        }
    }

    List<Control> EditorLines(string id)
    {
        var lines = new List<Control>();
        switch (id)
        {
            case ObsFilters.NoiseSuppress:
                lines.Add(Field("Yöntem", _nsMethod));
                lines.Add(Field("Bastırma", _nsLevel));
                lines.Add(Hint("RNNoise’ta seviye yok. 48 kHz değilse Speex kullanılır."));
                break;
            case ObsFilters.NoiseGate:
                lines.Add(Field("Açılış dB", _gateOpen));
                lines.Add(Field("Kapanış dB", _gateClose));
                lines.Add(Field("Atak ms", _gateAttack));
                lines.Add(Field("Tutma ms", _gateHold));
                lines.Add(Field("Bırakma ms", _gateRelease));
                break;
            case ObsFilters.Invert:
                lines.Add(Hint("Sesin işaretini ters çevirir."));
                break;
            case ObsFilters.Gain:
                lines.Add(Field("Kazan dB", _gain));
                break;
            case ObsFilters.Expander:
                lines.Add(Field("Hazır ayar", _expPreset));
                lines.Add(Field("Oran :1", _expRatio));
                lines.Add(Field("Eşik dB", _expThreshold));
                lines.Add(Field("Atak ms", _expAttack));
                lines.Add(Field("Bırakma ms", _expRelease));
                lines.Add(Field("Çıkış dB", _expOutput));
                lines.Add(Field("Algılama", _expDetector));
                break;
            case ObsFilters.Upward:
                lines.Add(Field("Oran :1", _upRatio));
                lines.Add(Field("Eşik dB", _upThreshold));
                lines.Add(Field("Atak ms", _upAttack));
                lines.Add(Field("Bırakma ms", _upRelease));
                lines.Add(Field("Çıkış dB", _upOutput));
                lines.Add(Field("Diz dB", _upKnee));
                break;
            case ObsFilters.Eq:
                lines.Add(Field("Low dB", _eqLow));
                lines.Add(Field("Mid dB", _eqMid));
                lines.Add(Field("High dB", _eqHigh));
                lines.Add(Hint("Geçişler 800 Hz ve 5 kHz. Kaydırıcı değiller."));
                break;
            case ObsFilters.Compressor:
                lines.Add(Field("Oran :1", _compRatio));
                lines.Add(Field("Eşik dB", _compThreshold));
                lines.Add(Field("Atak ms", _compAttack));
                lines.Add(Field("Bırakma ms", _compRelease));
                lines.Add(Field("Çıkış dB", _compOutput));
                lines.Add(Hint("Yan zincir yok."));
                break;
            case ObsFilters.Limiter:
                lines.Add(Field("Eşik dB", _limThreshold));
                lines.Add(Field("Bırakma ms", _limRelease));
                break;
        }

        return lines;
    }

    Control Card(string id, int index, int count)
    {
        bool on = ToggleFor(id).Checked;
        var card = new Panel
        {
            BorderStyle = BorderStyle.FixedSingle,
            BackColor = on ? CardBg : CardOff,
            Width = CardWidth(),
            Height = 36,
            Margin = new Padding(0, 0, 0, 4),
            Tag = id,
        };
        var toggle = ToggleFor(id);
        toggle.BackColor = card.BackColor;
        card.Controls.Add(toggle);
        var name = new Button
        {
            Text = ObsFilters.NameOf(id),
            FlatStyle = FlatStyle.Flat,
            TextAlign = ContentAlignment.MiddleLeft,
            ForeColor = Ink,
            BackColor = card.BackColor,
            Font = new Font(Font, FontStyle.Bold),
            Cursor = Cursors.Hand,
        };
        name.FlatAppearance.BorderSize = 0;
        name.FlatAppearance.MouseOverBackColor = Color.FromArgb(72, 52, 50);
        name.Click += (_, _) => OpenEditor(id);
        card.Controls.Add(name);
        if (index > 0)
        {
            var up = MiniButton("üst");
            up.Click += (_, _) => MoveFilter(id, -1);
            card.Controls.Add(up);
        }
        if (index < count - 1)
        {
            var down = MiniButton("alt");
            down.Click += (_, _) => MoveFilter(id, 1);
            card.Controls.Add(down);
        }
        var remove = MiniButton("Sil");
        remove.Click += (_, _) => RemoveFilter(id);
        card.Controls.Add(remove);
        LayoutRow(card, card.Width);
        return card;
    }

    static void LayoutRow(Control card, int width)
    {
        if (card.Controls.Count < 2)
            return;
        card.Controls[0].Location = new Point(6, 6);
        int right = width - 6;
        for (int i = card.Controls.Count - 1; i >= 2; i--)
        {
            right -= card.Controls[i].Width + 4;
            card.Controls[i].Location = new Point(right, 4);
        }
        card.Controls[1].SetBounds(58, 2, Math.Max(72, right - 62), 30);
    }

    void OpenEditor(string id)
    {
        var lines = EditorLines(id);
        using var dialog = new Form
        {
            Text = ObsFilters.NameOf(id),
            FormBorderStyle = FormBorderStyle.FixedDialog,
            StartPosition = FormStartPosition.CenterParent,
            MaximizeBox = false,
            MinimizeBox = false,
            ShowInTaskbar = false,
            BackColor = Bg,
            ForeColor = Ink,
            Font = Font,
        };
        int y = 12;
        foreach (var line in lines)
        {
            line.BackColor = Bg;
            foreach (Control bit in line.Controls)
            {
                if (bit is Label)
                    bit.ForeColor = Muted;
                bit.BackColor = bit is Label ? Bg : Input;
            }
            line.Location = new Point(12, y);
            dialog.Controls.Add(line);
            int height = Math.Max(line.Height, line.GetPreferredSize(new Size(300, 0)).Height);
            y += height + 4;
        }
        var close = SoftButton("Kapat");
        close.Location = new Point(12, y + 6);
        close.Click += (_, _) => dialog.Close();
        dialog.Controls.Add(close);
        dialog.ClientSize = new Size(340, close.Bottom + 14);
        dialog.ShowDialog(this);
        ParkInputs();
    }

    internal string AuditLayout()
    {
        PerformLayout();
        var problems = new List<string>();
        Control? previous = null;
        foreach (Control card in _filters.Controls)
        {
            if (previous != null && card.Top < previous.Bottom - 1)
                problems.Add($"kart binmesi {previous.Top}+{previous.Height} -> {card.Top}");
            foreach (Control child in card.Controls)
            {
                if (child.Bottom > card.ClientSize.Height + 1)
                    problems.Add($"{child.Text} alt {child.Bottom} > kart {card.ClientSize.Height}");
                if (child.Right > card.ClientSize.Width + 1)
                    problems.Add($"{child.Text} sağ {child.Right} > kart {card.ClientSize.Width}");
            }
            previous = card;
        }
        if (_filters.Controls.Count < 5)
            problems.Add("hazır zincir eksik");
        var shell = _scroll.Parent;
        if (shell != null)
        {
            foreach (Control part in shell.Controls)
            {
                if (part.Top < -1 || part.Bottom > shell.ClientSize.Height + 1)
                    problems.Add($"kenar taşması {part.Dock} {part.Top}+{part.Height}/{shell.ClientSize.Height}");
            }
        }
        return problems.Count == 0 ? "layout ok " + _filters.Controls.Count : string.Join("; ", problems);
    }

    static readonly JsonSerializerOptions JsonOptions = new() { WriteIndented = true, IncludeFields = true };

    static string RackPath => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "Mikraf", "rack.json");

    void LoadRack()
    {
        try
        {
            if (!File.Exists(RackPath))
                return;
            var loaded = JsonSerializer.Deserialize<RackSettings>(File.ReadAllText(RackPath), JsonOptions);
            if (loaded == null)
                return;
            Copy(loaded, _rack);
            _rack.EnsureChain();
        }
        catch
        {
            // Keep the OBS scene defaults baked into RackSettings.
        }
    }

    void SaveRack()
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(RackPath)!);
            File.WriteAllText(RackPath, JsonSerializer.Serialize(_rack, JsonOptions));
        }
        catch (Exception ex)
        {
            _status.Text = "Ayar kaydedilemedi: " + ex.Message;
        }
    }

    static void Copy(RackSettings from, RackSettings to)
    {
        var json = JsonSerializer.Serialize(from, JsonOptions);
        var copy = JsonSerializer.Deserialize<RackSettings>(json, JsonOptions);
        if (copy == null)
            return;
        foreach (var field in typeof(RackSettings).GetFields())
            field.SetValue(to, field.GetValue(copy));
    }

    static CheckBox ToggleBox() => new()
    {
        Text = "Açık",
        AutoSize = true,
        ForeColor = Ink,
        BackColor = CardBg,
        Margin = new Padding(0, 4, 6, 0),
    };

    static Button PrimaryButton(string text)
    {
        var button = SoftButton(text);
        button.BackColor = Accent;
        button.ForeColor = Color.FromArgb(42, 20, 18);
        button.FlatAppearance.BorderSize = 0;
        button.Font = new Font("Segoe UI", 9.5f, FontStyle.Bold);
        return button;
    }

    static Button SoftButton(string text) => SoftButton(new Button { Text = text, AutoSize = true });

    static Button MiniButton(string text)
    {
        var button = SoftButton(text);
        button.AutoSize = false;
        button.Size = new Size(40, 26);
        return button;
    }

    static Button SoftButton(Button button)
    {
        button.FlatStyle = FlatStyle.Flat;
        button.FlatAppearance.BorderSize = 1;
        button.FlatAppearance.BorderColor = LineCol;
        button.BackColor = PanelBg;
        button.ForeColor = Ink;
        button.Cursor = Cursors.Hand;
        button.Margin = new Padding(0, 2, 6, 2);
        return button;
    }

    static Label Label(string text) => new()
    {
        Text = text,
        AutoSize = true,
        ForeColor = Muted,
        Padding = new Padding(0, 6, 8, 0),
    };

    static Control Field(string name, Control editor)
    {
        var row = new Panel { Height = 32, Width = 310, BackColor = Bg };
        row.Controls.Add(new Label
        {
            Text = name,
            AutoSize = false,
            Bounds = new Rectangle(0, 6, 128, 20),
            ForeColor = Muted,
        });
        editor.Location = new Point(132, 2);
        row.Controls.Add(editor);
        return row;
    }

    static Label Hint(string text) => new()
    {
        Text = text,
        AutoSize = true,
        MaximumSize = new Size(340, 0),
        ForeColor = Muted,
        Font = new Font("Segoe UI", 8.25f),
        Padding = new Padding(0, 0, 0, 4),
    };

    CheckBox ToggleFor(string id) => id switch
    {
        ObsFilters.NoiseSuppress => _nsOn,
        ObsFilters.NoiseGate => _gateOn,
        ObsFilters.Invert => _invertOn,
        ObsFilters.Gain => _gainOn,
        ObsFilters.Expander => _expOn,
        ObsFilters.Upward => _upOn,
        ObsFilters.Eq => _eqOn,
        ObsFilters.Compressor => _compOn,
        ObsFilters.Limiter => _limOn,
        _ => _nsOn,
    };

    static void SetFlag(RackSettings rack, string id, bool on)
    {
        switch (id)
        {
            case ObsFilters.NoiseSuppress: rack.NoiseSuppress = on; break;
            case ObsFilters.NoiseGate: rack.NoiseGate = on; break;
            case ObsFilters.Invert: rack.Invert = on; break;
            case ObsFilters.Gain: rack.Gain = on; break;
            case ObsFilters.Expander: rack.Expander = on; break;
            case ObsFilters.Upward: rack.Upward = on; break;
            case ObsFilters.Eq: rack.Eq = on; break;
            case ObsFilters.Compressor: rack.Compressor = on; break;
            case ObsFilters.Limiter: rack.Limiter = on; break;
        }
    }

    void SetFlag(string id, bool on) => SetFlag(_rack, id, on);

    static FlowLayoutPanel Line(params Control[] controls)
    {
        var row = new FlowLayoutPanel
        {
            FlowDirection = FlowDirection.LeftToRight,
            WrapContents = false,
            AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink,
            Margin = Padding.Empty,
            Padding = Padding.Empty,
        };
        row.Controls.AddRange(controls);
        var preferred = row.GetPreferredSize(Size.Empty);
        row.AutoSize = false;
        row.Size = new Size(Math.Max(preferred.Width, 40), Math.Max(preferred.Height, 28));
        return row;
    }

    sealed record DeviceItem(string Id, string Name)
    {
        public override string ToString() => Name;
    }

    sealed record FilterChoice(string Id, string Name)
    {
        public override string ToString() => Name;
    }
}
