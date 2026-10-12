#include "dsp.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audioenginebaseapo.h>
#include <audioengineextensionapo.h>
#include <audiomediatype.h>
#include <propvarutil.h>
#include <strsafe.h>
#include <aclapi.h>
#include <sddl.h>
#include <tlhelp32.h>
#include <cfgmgr32.h>
#include <stdio.h>
#include <wchar.h>
#include <new>

// {7C3E9A21-5B14-4E6A-9F02-6D8A4C1E2B70}
static const GUID CLSID_MikrafApo = { 0x7c3e9a21, 0x5b14, 0x4e6a, { 0x9f, 0x02, 0x6d, 0x8a, 0x4c, 0x1e, 0x2b, 0x70 } };
static const wchar_t kClsid[] = L"{7C3E9A21-5B14-4E6A-9F02-6D8A4C1E2B70}";
static const wchar_t kMapName[] = L"Global\\MikrafApoV1";
static const wchar_t kDir[] = L"C:\\ProgramData\\Mikraf";
static const wchar_t kSettings[] = L"C:\\ProgramData\\Mikraf\\apo-settings.bin";
static const wchar_t kDeviceFile[] = L"C:\\ProgramData\\Mikraf\\apo-device.txt";
static const wchar_t kResult[] = L"C:\\ProgramData\\Mikraf\\install-result.txt";
static const wchar_t kLogPath[] = L"C:\\ProgramData\\Mikraf\\install.log";
static const wchar_t kBackup[] = L"C:\\ProgramData\\Mikraf\\fx-backup.bin";
static const wchar_t kVoiceBackup[] = L"C:\\ProgramData\\Mikraf\\vc-backup.bin";
static const wchar_t kInstallDir[] = L"C:\\Program Files\\Mikraf";
// Effect id, not a COM class. Windows 11 asks for this list and crashes if the slot is empty.
static const GUID kEffectId = { 0xa1c3e4b2, 0x6d18, 0x4f05, { 0x9a, 0x77, 0x2b, 0x4e, 0x8c, 0x15, 0xd0, 0x61 } };
static const GUID kFloat = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
static const GUID kPcm = { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

static const PROPERTYKEY kFxStream = { { 0xD04E05A6, 0x594B, 0x4fb6, { 0xA8, 0x0D, 0x01, 0xAF, 0x5E, 0xED, 0x7D, 0x1D } }, 5 };
static const PROPERTYKEY kFxFriendly = { { 0xD04E05A6, 0x594B, 0x4fb6, { 0xA8, 0x0D, 0x01, 0xAF, 0x5E, 0xED, 0x7D, 0x1D } }, 4 };
static const PROPERTYKEY kFxCompositeStream = { { 0xD04E05A6, 0x594B, 0x4fb6, { 0xA8, 0x0D, 0x01, 0xAF, 0x5E, 0xED, 0x7D, 0x1D } }, 13 };
static const PROPERTYKEY kSfxModes = { { 0xd3993a3f, 0x99c2, 0x4402, { 0xb5, 0xec, 0xa9, 0x2a, 0x03, 0x67, 0x66, 0x4b } }, 5 };
static const PROPERTYKEY kDisableSysFx = { { 0x1da5d803, 0xd492, 0x4edd, { 0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e } }, 5 };

static void ApoTrace(const wchar_t *text);

static HMODULE g_module;
static HMODULE g_speex;
static MikrafSpxOpen g_spxOpen;
static MikrafSpxClose g_spxClose;
static MikrafSpxRun g_spxRun;

static SRWLOCK g_speexLock = SRWLOCK_INIT;
static bool g_speexTried;

// Instances lock on different engine threads; the load must run once and never free a module another thread is using.
static void LoadSpeex()
{
    wchar_t path[MAX_PATH];
    wchar_t *slash;
    HMODULE module;
    MikrafSpxOpen open;
    MikrafSpxClose close;
    MikrafSpxRun run;
    AcquireSRWLockExclusive(&g_speexLock);
    if (g_speexTried) {
        ReleaseSRWLockExclusive(&g_speexLock);
        return;
    }
    g_speexTried = true;
    if (!GetModuleFileNameW(g_module, path, MAX_PATH))
        goto done;
    slash = wcsrchr(path, L'\\');
    if (!slash)
        goto done;
    if (wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"mikraf-speex.dll") != 0)
        goto done;
    module = LoadLibraryW(path);
    if (!module)
        goto done;
    open = (MikrafSpxOpen)GetProcAddress(module, "mikraf_spx_open");
    close = (MikrafSpxClose)GetProcAddress(module, "mikraf_spx_close");
    run = (MikrafSpxRun)GetProcAddress(module, "mikraf_spx_run");
    if (!open || !close || !run) {
        FreeLibrary(module);
        goto done;
    }
    g_speex = module;
    g_spxClose = close;
    g_spxRun = run;
    g_spxOpen = open;
done:
    ReleaseSRWLockExclusive(&g_speexLock);
}

static SECURITY_ATTRIBUTES *MapSecurity()
{
    static SECURITY_DESCRIPTOR sd;
    static SECURITY_ATTRIBUTES sa;
    InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE);
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = &sd;
    sa.bInheritHandle = FALSE;
    return &sa;
}

static void AllowFolder(const wchar_t *path)
{
    PSID users = NULL;
    PSID service = NULL;
    EXPLICIT_ACCESSW ea[2];
    PACL acl = NULL;
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    wchar_t mutablePath[MAX_PATH];
    ZeroMemory(ea, sizeof(ea));
    if (!AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_USERS, 0, 0, 0, 0, 0, 0, &users))
        return;
    if (!AllocateAndInitializeSid(&nt, 1, SECURITY_LOCAL_SERVICE_RID, 0, 0, 0, 0, 0, 0, 0, &service)) {
        FreeSid(users);
        return;
    }
    ea[0].grfAccessPermissions = FILE_ALL_ACCESS;
    ea[0].grfAccessMode = GRANT_ACCESS;
    ea[0].grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    ea[0].Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea[0].Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea[0].Trustee.ptstrName = (LPWSTR)users;
    ea[1] = ea[0];
    ea[1].Trustee.ptstrName = (LPWSTR)service;
    ea[1].Trustee.TrusteeType = TRUSTEE_IS_USER;
    if (SetEntriesInAclW(2, ea, NULL, &acl) == ERROR_SUCCESS) {
        StringCchCopyW(mutablePath, MAX_PATH, path);
        SetNamedSecurityInfoW(mutablePath, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, acl, NULL);
        LocalFree(acl);
    }
    FreeSid(users);
    FreeSid(service);
}

static int LoadPayload(MikrafApoPayload *payload)
{
    FILE *file = NULL;
    uint32_t magic = 0;
    uint32_t version = 0;
    int ok;
    if (_wfopen_s(&file, kSettings, L"rb") != 0 || !file)
        return 0;
    ok = fread(&magic, 4, 1, file) == 1 && fread(&version, 4, 1, file) == 1
        && magic == MIKRAF_APO_MAGIC && version == MIKRAF_APO_VERSION
        && fread(payload, sizeof(*payload), 1, file) == 1;
    fclose(file);
    return ok;
}

static void SavePayload(const MikrafApoPayload *payload)
{
    FILE *file = NULL;
    uint32_t magic = MIKRAF_APO_MAGIC;
    uint32_t version = MIKRAF_APO_VERSION;
    CreateDirectoryW(kDir, NULL);
    AllowFolder(kDir);
    if (_wfopen_s(&file, kSettings, L"wb") != 0 || !file)
        return;
    fwrite(&magic, 4, 1, file);
    fwrite(&version, 4, 1, file);
    fwrite(payload, sizeof(*payload), 1, file);
    fclose(file);
}

static HANDLE OpenBlock(int create, MikrafApoBlock **view)
{
    HANDLE handle;
    DWORD err = ERROR_ALREADY_EXISTS;
    MikrafApoBlock *block;
    handle = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, kMapName);
    if (!handle && create) {
        SetLastError(0);
        handle = CreateFileMappingW(INVALID_HANDLE_VALUE, MapSecurity(), PAGE_READWRITE, 0, (DWORD)sizeof(MikrafApoBlock), kMapName);
        err = GetLastError();
    }
    if (!handle)
        return NULL;
    block = (MikrafApoBlock *)MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(MikrafApoBlock));
    if (!block) {
        CloseHandle(handle);
        return NULL;
    }
    if (err != ERROR_ALREADY_EXISTS || block->magic != MIKRAF_APO_MAGIC || block->version != MIKRAF_APO_VERSION) {
        ZeroMemory(block, sizeof(*block));
        block->magic = MIKRAF_APO_MAGIC;
        block->version = MIKRAF_APO_VERSION;
        mikraf_settings_default(&block->payload);
        if (!LoadPayload(&block->payload))
            mikraf_settings_default(&block->payload);
    }
    *view = block;
    return handle;
}

static void ReadPayload(MikrafApoBlock *block, MikrafApoPayload *out)
{
    int spins = 0;
    if (!block || block->magic != MIKRAF_APO_MAGIC) {
        mikraf_settings_default(out);
        return;
    }
    for (;;) {
        uint32_t a = block->seq;
        if (a & 1u) {
            if (++spins > 64) {
                *out = block->payload;
                return;
            }
            continue;
        }
        MemoryBarrier();
        *out = block->payload;
        MemoryBarrier();
        if (a == block->seq)
            return;
    }
}

static int EndpointGuid(const wchar_t *deviceId, wchar_t *guid, size_t guidChars)
{
    const wchar_t *start = deviceId ? wcsrchr(deviceId, L'{') : NULL;
    const wchar_t *end = start ? wcschr(start, L'}') : NULL;
    size_t count;
    if (!start || !end)
        return 0;
    count = (size_t)(end - start + 1);
    if (count + 1 > guidChars)
        return 0;
    wmemcpy(guid, start, count);
    guid[count] = 0;
    return 1;
}

class MikrafApo final : public IAudioProcessingObject, public IAudioProcessingObjectRT, public IAudioProcessingObjectConfiguration, public IAudioSystemEffects3
{
public:
    class Identity final : public IUnknown
    {
    public:
        MikrafApo *self;
        STDMETHOD(QueryInterface)(REFIID riid, void **ppv) { return self->OwnQuery(riid, ppv); }
        STDMETHOD_(ULONG, AddRef)() { return (ULONG)InterlockedIncrement(&self->refs); }
        STDMETHOD_(ULONG, Release)()
        {
            ULONG left = (ULONG)InterlockedDecrement(&self->refs);
            if (left == 0) {
                self->StopPoll();
                self->UnlockForProcess();
                if (self->block)
                    UnmapViewOfFile(self->block);
                if (self->map)
                    CloseHandle(self->map);
                delete self;
            }
            return left;
        }
    };

    MikrafApo(IUnknown *controller = nullptr) : refs(1), ready(false), locked(false), rate(0), channels(0), maxFrames(0), sampleBytes(4), pcmBits(0), scratch(NULL), map(NULL), block(NULL), outer(controller)
    {
        identity.self = this;
        ZeroMemory(&dsp, sizeof(dsp));
    }

    IUnknown *Controlling()
    {
        return outer ? outer : static_cast<IUnknown *>(&identity);
    }

    HRESULT OwnQuery(REFIID riid, void **ppv)
    {
        if (!ppv)
            return E_POINTER;
        if (riid == __uuidof(IUnknown)) {
            *ppv = static_cast<IUnknown *>(&identity);
            InterlockedIncrement(&refs);
            return S_OK;
        }
        if (riid == __uuidof(IAudioProcessingObject))
            *ppv = static_cast<IAudioProcessingObject *>(this);
        else if (riid == __uuidof(IAudioProcessingObjectRT))
            *ppv = static_cast<IAudioProcessingObjectRT *>(this);
        else if (riid == __uuidof(IAudioProcessingObjectConfiguration))
            *ppv = static_cast<IAudioProcessingObjectConfiguration *>(this);
        else if (riid == __uuidof(IAudioSystemEffects) || riid == __uuidof(IAudioSystemEffects2) || riid == __uuidof(IAudioSystemEffects3))
            *ppv = static_cast<IAudioSystemEffects3 *>(this);
        else {
            *ppv = NULL;
            return E_NOINTERFACE;
        }
        // Interface pointers count against the controlling unknown, so an aggregating caller releases what it adds.
        AddRef();
        return S_OK;
    }

    STDMETHOD(QueryInterface)(REFIID riid, void **ppv) { return Controlling()->QueryInterface(riid, ppv); }
    STDMETHOD_(ULONG, AddRef)() { return Controlling()->AddRef(); }
    STDMETHOD_(ULONG, Release)() { return Controlling()->Release(); }
    void ReleaseConstructor() { identity.Release(); }

    // audiodg may be unable to create or open the Global mapping, so the settings file is also watched.
    bool RefreshFile()
    {
        WIN32_FILE_ATTRIBUTE_DATA info;
        MikrafApoPayload fresh;
        LONG next;
        if (!GetFileAttributesExW(kSettings, GetFileExInfoStandard, &info))
            return false;
        if (fileActive >= 0 && CompareFileTime(&info.ftLastWriteTime, &fileStamp) == 0)
            return false;
        if (!LoadPayload(&fresh))
            return false;
        next = fileActive == 0 ? 1 : 0;
        filePayload[next] = fresh;
        InterlockedExchange(&fileActive, next);
        fileStamp = info.ftLastWriteTime;
        return true;
    }

    static DWORD WINAPI PollMain(LPVOID param)
    {
        MikrafApo *self = (MikrafApo *)param;
        while (WaitForSingleObject(self->stopEvent, 150) == WAIT_TIMEOUT)
            self->RefreshFile();
        return 0;
    }

    void StartPoll()
    {
        RefreshFile();
        stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (stopEvent)
            pollThread = CreateThread(NULL, 0, PollMain, this, 0, NULL);
    }

    void StopPoll()
    {
        if (pollThread) {
            SetEvent(stopEvent);
            WaitForSingleObject(pollThread, 2000);
            CloseHandle(pollThread);
            pollThread = NULL;
        }
        if (stopEvent) {
            CloseHandle(stopEvent);
            stopEvent = NULL;
        }
    }

    void ReadSettings(MikrafApoPayload *out)
    {
        LONG active = fileActive;
        if (active >= 0) {
            *out = filePayload[active];
            return;
        }
        ReadPayload(block, out);
    }

    STDMETHOD(Reset)()
    {
        if (!ready)
            return APOERR_NOT_INITIALIZED;
        AcquireSRWLockExclusive(&procLock);
        if (locked && !dsp_prepare(&dsp, rate, channels, maxFrames, g_spxOpen, g_spxClose, g_spxRun))
            locked = false;
        ReleaseSRWLockExclusive(&procLock);
        return S_OK;
    }

    STDMETHOD(GetLatency)(HNSTIME *pTime)
    {
        MikrafApoPayload settings;
        int frame;
        if (!pTime)
            return E_POINTER;
        ReadSettings(&settings);
        frame = rate > 0 ? rate / 100 : 480;
        if (settings.nsOn && g_spxRun && frame > 0 && frame <= MIKRAF_DSP_FRAME_CAP && rate > 0)
            *pTime = (HNSTIME)frame * 10000000LL / rate;
        else
            *pTime = 0;
        return S_OK;
    }

    STDMETHOD(GetRegistrationProperties)(APO_REG_PROPERTIES **ppRegProps)
    {
        APO_REG_PROPERTIES *props;
        if (!ppRegProps)
            return E_POINTER;
        props = (APO_REG_PROPERTIES *)CoTaskMemAlloc(sizeof(APO_REG_PROPERTIES));
        if (!props)
            return E_OUTOFMEMORY;
        ZeroMemory(props, sizeof(*props));
        props->clsid = CLSID_MikrafApo;
        // Must-match flags make this Windows build call a null converter for 16-bit or stereo opens.
        props->Flags = APO_FLAG_NONE;
        StringCchCopyW(props->szFriendlyName, 256, L"Mikraf");
        StringCchCopyW(props->szCopyrightInfo, 256, L"Mikraf");
        props->u32MajorVersion = 1;
        props->u32MinorVersion = 0;
        props->u32MinInputConnections = 1;
        props->u32MaxInputConnections = 1;
        props->u32MinOutputConnections = 1;
        props->u32MaxOutputConnections = 1;
        props->u32MaxInstances = 0xffffffffu;
        props->u32NumAPOInterfaces = 1;
        props->iidAPOInterfaceList[0] = __uuidof(IAudioSystemEffects);
        *ppRegProps = props;
        return S_OK;
    }

    STDMETHOD(Initialize)(UINT32 cbDataSize, BYTE *pbyData)
    {
        APOInitBaseStruct *base;
        if (ready)
            return APOERR_ALREADY_INITIALIZED;
        if (!pbyData || cbDataSize < sizeof(APOInitBaseStruct))
            return E_INVALIDARG;
        base = (APOInitBaseStruct *)pbyData;
        if (base->clsid != CLSID_MikrafApo)
            ApoTrace(L"Initialize other clsid");
        map = OpenBlock(1, &block);
        if (!block) {
            wchar_t line[64];
            StringCchPrintfW(line, 64, L"no shared block err %lu", GetLastError());
            ApoTrace(line);
        }
        StartPoll();
        ready = true;
        ApoTrace(L"Initialize ok");
        return S_OK;
    }

    STDMETHOD(IsInputFormatSupported)(IAudioMediaType *opposite, IAudioMediaType *requested, IAudioMediaType **supported)
    {
        UNREFERENCED_PARAMETER(opposite);
        return CheckFormat(requested, supported);
    }

    STDMETHOD(IsOutputFormatSupported)(IAudioMediaType *opposite, IAudioMediaType *requested, IAudioMediaType **supported)
    {
        UNREFERENCED_PARAMETER(opposite);
        return CheckFormat(requested, supported);
    }

    STDMETHOD(GetInputChannelCount)(UINT32 *count)
    {
        if (!count)
            return E_POINTER;
        // Before lock, 1 under-reports a stereo open and the engine sizes a buffer we then overrun.
        *count = channels > 0 ? (UINT32)channels : 2u;
        return S_OK;
    }

    STDMETHOD_(void, APOProcess)(UINT32 inputCount, APO_CONNECTION_PROPERTY **inputs, UINT32 outputCount, APO_CONNECTION_PROPERTY **outputs)
    {
        // The real-time thread never waits: if a lock/reset owns the buffers, this block passes through.
        if (TryAcquireSRWLockShared(&procLock)) {
            ProcessSafe(this, inputCount, inputs, outputCount, outputs);
            ReleaseSRWLockShared(&procLock);
            return;
        }
        PassThrough(inputCount, inputs, outputCount, outputs);
    }

    STDMETHOD_(UINT32, CalcInputFrames)(UINT32 outputFrames) { return outputFrames; }
    STDMETHOD_(UINT32, CalcOutputFrames)(UINT32 inputFrames) { return inputFrames; }

    STDMETHOD(LockForProcess)(UINT32 inputCount, APO_CONNECTION_DESCRIPTOR **inputs, UINT32 outputCount, APO_CONNECTION_DESCRIPTOR **outputs)
    {
        HRESULT result;
        AcquireSRWLockExclusive(&procLock);
        result = LockLocked(inputCount, inputs, outputCount, outputs);
        ReleaseSRWLockExclusive(&procLock);
        return result;
    }

    HRESULT LockLocked(UINT32 inputCount, APO_CONNECTION_DESCRIPTOR **inputs, UINT32 outputCount, APO_CONNECTION_DESCRIPTOR **outputs)
    {
        UNCOMPRESSEDAUDIOFORMAT format;
        HRESULT hr;
        UINT32 frames;
        if (!ready)
            return APOERR_NOT_INITIALIZED;
        locked = false;
        if (inputCount != 1 || outputCount != 1 || !inputs || !outputs || !inputs[0] || !outputs[0])
            return APOERR_NUM_CONNECTIONS_INVALID;
        if (!inputs[0]->pFormat)
            return APOERR_INVALID_CONNECTION_FORMAT;
        hr = inputs[0]->pFormat->GetUncompressedAudioFormat(&format);
        if (FAILED(hr))
            return hr;
        if (!FormatAccepted(format))
            return APOERR_FORMAT_NOT_SUPPORTED;
        ApoTrace(L"LockForProcess");
        frames = outputs[0]->u32MaxFrameCount;
        if (inputs[0]->u32MaxFrameCount > frames)
            frames = inputs[0]->u32MaxFrameCount;
        if (frames < 480)
            frames = 480;
        if (frames > 8192)
            frames = 8192;
        LoadSpeex();
        rate = (int)format.fFramesPerSecond;
        channels = (int)format.dwSamplesPerFrame;
        maxFrames = (int)frames;
        pcmBits = PcmBits(format);
        sampleBytes = pcmBits == 16 ? 2 : 4;
        free(scratch);
        scratch = NULL;
        if (pcmBits != 0) {
            scratch = (float *)malloc(sizeof(float) * (size_t)maxFrames * (size_t)channels);
            if (!scratch)
                return E_OUTOFMEMORY;
        }
        if (!dsp_prepare(&dsp, rate, channels, maxFrames, g_spxOpen, g_spxClose, g_spxRun)) {
            free(scratch);
            scratch = NULL;
            return E_OUTOFMEMORY;
        }
        if (block) {
            block->sampleRate = (uint32_t)rate;
            block->channelCount = (uint32_t)channels;
        }
        {
            wchar_t line[200];
            StringCchPrintfW(line, 200, L"lock rate %d ch %d bits %d pcm %d max %d in %lu out %lu ns %d",
                rate, channels, (int)format.dwValidBitsPerSample, pcmBits, maxFrames,
                (unsigned long)inputs[0]->u32MaxFrameCount, (unsigned long)outputs[0]->u32MaxFrameCount, dsp.frame);
            ApoTrace(line);
        }
        traced = false;
        locked = true;
        return S_OK;
    }

    STDMETHOD(UnlockForProcess)()
    {
        AcquireSRWLockExclusive(&procLock);
        locked = false;
        dsp_release(&dsp);
        free(scratch);
        scratch = NULL;
        ReleaseSRWLockExclusive(&procLock);
        return S_OK;
    }

    STDMETHOD(GetEffectsList)(LPGUID *ids, UINT *count, HANDLE event)
    {
        UNREFERENCED_PARAMETER(event);
        LPGUID list;
        if (!ids || !count)
            return E_POINTER;
        list = (LPGUID)CoTaskMemAlloc(sizeof(GUID));
        if (!list)
            return E_OUTOFMEMORY;
        *list = kEffectId;
        *ids = list;
        *count = 1;
        return S_OK;
    }

    STDMETHOD(GetControllableSystemEffectsList)(AUDIO_SYSTEMEFFECT **effects, UINT *numEffects, HANDLE event)
    {
        AUDIO_SYSTEMEFFECT *list;
        UNREFERENCED_PARAMETER(event);
        if (!effects || !numEffects)
            return E_POINTER;
        list = (AUDIO_SYSTEMEFFECT *)CoTaskMemAlloc(sizeof(AUDIO_SYSTEMEFFECT));
        if (!list)
            return E_OUTOFMEMORY;
        ZeroMemory(list, sizeof(*list));
        list->id = kEffectId;
        list->canSetState = FALSE;
        list->state = AUDIO_SYSTEMEFFECT_STATE_ON;
        *effects = list;
        *numEffects = 1;
        return S_OK;
    }

    STDMETHOD(SetAudioSystemEffectState)(GUID effectId, AUDIO_SYSTEMEFFECT_STATE state)
    {
        UNREFERENCED_PARAMETER(effectId);
        UNREFERENCED_PARAMETER(state);
        return S_OK;
    }

    void Process(UINT32 inputCount, APO_CONNECTION_PROPERTY **inputs, UINT32 outputCount, APO_CONNECTION_PROPERTY **outputs)
    {
        APO_CONNECTION_PROPERTY *inConn;
        APO_CONNECTION_PROPERTY *outConn;
        UINT32 frames;
        UINT32 offset = 0;
        int silent;
        const float *src;
        float *dst;
        MikrafApoPayload settings;
        BYTE *rawOut;
        const BYTE *rawIn;
        if (inputCount < 1 || outputCount < 1 || !inputs || !outputs || !inputs[0] || !outputs[0])
            return;
        inConn = inputs[0];
        outConn = outputs[0];
        frames = inConn->u32ValidFrameCount;
        outConn->u32ValidFrameCount = frames;
        outConn->u32BufferFlags = BUFFER_VALID;
        outConn->u32Signature = APO_CONNECTION_PROPERTY_SIGNATURE;
        if (frames == 0 || !outConn->pBuffer)
            return;
        silent = inConn->u32BufferFlags == BUFFER_SILENT || inConn->pBuffer == 0;
        rawOut = (BYTE *)outConn->pBuffer;
        rawIn = silent ? NULL : (const BYTE *)inConn->pBuffer;
        if (!locked || channels <= 0 || (pcmBits != 0 && !scratch) || (!silent && !rawIn)) {
            // Cannot process: hand the audio through untouched instead of leaving stale bytes in the output.
            if (!silent && rawIn && rawIn != rawOut && channels > 0)
                memmove(rawOut, rawIn, (size_t)frames * (size_t)channels * (size_t)sampleBytes);
            else if (silent && channels > 0)
                ZeroMemory(rawOut, (size_t)frames * (size_t)channels * (size_t)sampleBytes);
            outConn->u32BufferFlags = BUFFER_VALID;
            return;
        }
        ReadSettings(&settings);
        if (!traced) {
            wchar_t line[96];
            traced = true;
            StringCchPrintfW(line, 96, L"first block frames %lu silent %d", (unsigned long)frames, silent);
            ApoTrace(line);
        }
        while (offset < frames) {
            UINT32 count = frames - offset;
            size_t samples;
            if ((int)count > maxFrames)
                count = (UINT32)maxFrames;
            samples = (size_t)count * (size_t)channels;
            if (pcmBits == 0) {
                dst = (float *)rawOut + (size_t)offset * (size_t)channels;
                src = silent ? NULL : (const float *)rawIn + (size_t)offset * (size_t)channels;
                if (silent) {
                    ZeroMemory(dst, sizeof(float) * samples);
                    dsp_process(&dsp, &settings, dst, dst, (int)count);
                } else {
                    dsp_process(&dsp, &settings, src, dst, (int)count);
                }
            } else {
                if (silent)
                    ZeroMemory(scratch, sizeof(float) * samples);
                else
                    PcmToFloat(rawIn + (size_t)offset * (size_t)channels * (size_t)sampleBytes, scratch, (int)samples);
                dsp_process(&dsp, &settings, scratch, scratch, (int)count);
                FloatToPcm(scratch, rawOut + (size_t)offset * (size_t)channels * (size_t)sampleBytes, (int)samples);
            }
            offset += count;
        }
        if (block)
            InterlockedIncrement((volatile LONG *)&block->processCount);
    }

private:
    static HRESULT CheckFormat(IAudioMediaType *requested, IAudioMediaType **supported)
    {
        UNCOMPRESSEDAUDIOFORMAT format;
        HRESULT hr;
        if (!supported)
            return E_POINTER;
        *supported = NULL;
        if (!requested)
            return E_POINTER;
        hr = requested->GetUncompressedAudioFormat(&format);
        if (FAILED(hr))
            return hr;
        if (FormatAccepted(format)) {
            if (!IsEqualGUID(format.guidFormatType, kFloat))
                ApoTrace(L"pcm accepted");
            *supported = requested;
            requested->AddRef();
            return S_OK;
        }
        {
            wchar_t line[160];
            StringCchPrintfW(line, 160, L"fmt rejected %08lX ch %lu bits %lu/%lu rate %.0f",
                (unsigned long)format.guidFormatType.Data1,
                (unsigned long)format.dwSamplesPerFrame,
                (unsigned long)format.dwValidBitsPerSample,
                (unsigned long)format.dwBytesPerSampleContainer,
                format.fFramesPerSecond);
            ApoTrace(line);
        }
        return APOERR_FORMAT_NOT_SUPPORTED;
    }

    static bool FormatAccepted(const UNCOMPRESSEDAUDIOFORMAT &format)
    {
        if (format.dwSamplesPerFrame < 1 || format.dwSamplesPerFrame > MIKRAF_DSP_MAX_CHANNELS)
            return false;
        if (format.fFramesPerSecond < 8000.f || format.fFramesPerSecond > 192000.f)
            return false;
        if (IsEqualGUID(format.guidFormatType, kFloat))
            return format.dwBytesPerSampleContainer == 4 && format.dwValidBitsPerSample == 32;
        if (IsEqualGUID(format.guidFormatType, kPcm))
            return (format.dwBytesPerSampleContainer == 2 && format.dwValidBitsPerSample == 16)
                || (format.dwBytesPerSampleContainer == 4 && format.dwValidBitsPerSample == 32);
        return false;
    }

    static int PcmBits(const UNCOMPRESSEDAUDIOFORMAT &format)
    {
        if (IsEqualGUID(format.guidFormatType, kPcm) && format.dwBytesPerSampleContainer == 2)
            return 16;
        if (IsEqualGUID(format.guidFormatType, kPcm))
            return 32;
        return 0;
    }

    void PcmToFloat(const BYTE *src, float *dst, int samples) const
    {
        int i;
        if (pcmBits == 16) {
            const int16_t *in = (const int16_t *)src;
            for (i = 0; i < samples; i++)
                dst[i] = in[i] / 32768.f;
            return;
        }
        const int32_t *in = (const int32_t *)src;
        for (i = 0; i < samples; i++)
            dst[i] = in[i] / 2147483648.f;
    }

    void FloatToPcm(const float *src, BYTE *dst, int samples) const
    {
        int i;
        for (i = 0; i < samples; i++) {
            float x = src[i];
            if (x > 1.f)
                x = 1.f;
            if (x < -1.f)
                x = -1.f;
            if (pcmBits == 16)
                ((int16_t *)dst)[i] = (int16_t)(x * 32767.f);
            else
                ((int32_t *)dst)[i] = (int32_t)(x * 2147483647.f);
        }
    }

    void PassThrough(UINT32 inputCount, APO_CONNECTION_PROPERTY **inputs, UINT32 outputCount, APO_CONNECTION_PROPERTY **outputs)
    {
        APO_CONNECTION_PROPERTY *in;
        APO_CONNECTION_PROPERTY *out;
        if (inputCount < 1 || outputCount < 1 || !inputs || !outputs || !inputs[0] || !outputs[0])
            return;
        in = inputs[0];
        out = outputs[0];
        out->u32ValidFrameCount = in->u32ValidFrameCount;
        out->u32BufferFlags = in->u32BufferFlags;
        out->u32Signature = APO_CONNECTION_PROPERTY_SIGNATURE;
        if (in->u32BufferFlags != BUFFER_SILENT && in->pBuffer && out->pBuffer && in->pBuffer != out->pBuffer && channels > 0)
            memcpy((void *)out->pBuffer, (const void *)in->pBuffer, (size_t)in->u32ValidFrameCount * (size_t)channels * (size_t)sampleBytes);
    }

    static void ProcessSafe(MikrafApo *self, UINT32 inputCount, APO_CONNECTION_PROPERTY **inputs, UINT32 outputCount, APO_CONNECTION_PROPERTY **outputs)
    {
        __try {
            self->Process(inputCount, inputs, outputCount, outputs);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            if (outputCount >= 1 && outputs && outputs[0] && inputs && inputs[0] && outputs[0]->pBuffer && inputs[0]->pBuffer
                && self->channels > 0 && inputs[0]->u32BufferFlags != BUFFER_SILENT) {
                memcpy((void *)outputs[0]->pBuffer, (const void *)inputs[0]->pBuffer, (size_t)self->sampleBytes * inputs[0]->u32ValidFrameCount * (size_t)self->channels);
            }
            if (outputCount >= 1 && outputs && outputs[0]) {
                outputs[0]->u32ValidFrameCount = inputs && inputs[0] ? inputs[0]->u32ValidFrameCount : 0;
                outputs[0]->u32BufferFlags = BUFFER_VALID;
                outputs[0]->u32Signature = APO_CONNECTION_PROPERTY_SIGNATURE;
            }
        }
    }

    volatile long refs;
    bool ready;
    bool locked;
    bool traced = false;
    MikrafApoPayload filePayload[2];
    volatile LONG fileActive = -1;
    FILETIME fileStamp = {};
    HANDLE pollThread = NULL;
    HANDLE stopEvent = NULL;
    int rate;
    int channels;
    int maxFrames;
    int sampleBytes;
    int pcmBits;
    float *scratch;
    HANDLE map;
    MikrafApoBlock *block;
    IUnknown *outer;
    Identity identity;
    SRWLOCK procLock = SRWLOCK_INIT;
    Dsp dsp;
};

class Factory final : public IClassFactory
{
public:
    STDMETHOD(QueryInterface)(REFIID riid, void **ppv)
    {
        if (!ppv)
            return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IClassFactory)) {
            *ppv = static_cast<IClassFactory *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() { return 2; }
    STDMETHOD_(ULONG, Release)() { return 1; }
    STDMETHOD(CreateInstance)(IUnknown *outer, REFIID riid, void **ppv)
    {
        MikrafApo *apo;
        HRESULT hr;
        if (outer && riid != __uuidof(IUnknown)) {
            ApoTrace(L"noagg");
            return CLASS_E_NOAGGREGATION;
        }
        if (!ppv)
            return E_POINTER;
        apo = new (std::nothrow) MikrafApo(outer);
        if (!apo)
            return E_OUTOFMEMORY;
        hr = apo->OwnQuery(riid, ppv);
        apo->ReleaseConstructor();
        if (FAILED(hr))
            return hr;
        ApoTrace(outer ? L"apo aggregated" : L"apo alone");
        return hr;
    }
    STDMETHOD(LockServer)(BOOL lock) { UNREFERENCED_PARAMETER(lock); return S_OK; }
};

static Factory g_factory;

static const GUID CLSID_VoiceClarity = { 0x9e6136e0, 0x57ab, 0x4949, { 0xb5, 0x7a, 0x36, 0x27, 0xbe, 0x14, 0x28, 0x55 } };
static const wchar_t kVcDllBackup[] = L"C:\\ProgramData\\Mikraf\\vc-dll-path.bin";

static int ReadVcDllPath(wchar_t *path, int count)
{
    FILE *file = NULL;
    char magic[8];
    DWORD size = 0;
    path[0] = 0;
    if (_wfopen_s(&file, kVcDllBackup, L"rb") != 0 || !file)
        return 0;
    if (fread(magic, 1, 8, file) != 8 || memcmp(magic, "VCDLL001", 8) != 0 || fread(&size, 4, 1, file) != 1) {
        fclose(file);
        return 0;
    }
    fseek(file, 4, SEEK_CUR);
    if (size < sizeof(wchar_t) || size >= (DWORD)count * sizeof(wchar_t) || fread(path, 1, size, file) != size) {
        fclose(file);
        path[0] = 0;
        return 0;
    }
    fclose(file);
    path[size / sizeof(wchar_t)] = 0;
    return 1;
}

static int StoreMentionsFifine(IPropertyStore *store)
{
    DWORD count = 0;
    int found = 0;
    if (!store || FAILED(store->GetCount(&count)))
        return 0;
    for (DWORD i = 0; i < count && i < 80; i++) {
        PROPERTYKEY key;
        PROPVARIANT value;
        const wchar_t *text = NULL;
        PropVariantInit(&value);
        if (FAILED(store->GetAt(i, &key)) || FAILED(store->GetValue(key, &value))) {
            PropVariantClear(&value);
            continue;
        }
        if (value.vt == VT_LPWSTR)
            text = value.pwszVal;
        else if (value.vt == VT_BSTR)
            text = value.bstrVal;
        if (text && i < 6) {
            wchar_t line[320];
            StringCchPrintfW(line, 320, L"prop %08lX,%lu %s", (unsigned long)key.fmtid.Data1, key.pid, text);
            ApoTrace(line);
        }
        if (text && (wcsstr(text, L"VID_3142&PID_A601") || wcsstr(text, L"fifine") || wcsstr(text, L"Fifine")))
            found = 1;
        PropVariantClear(&value);
    }
    return found;
}

static int ScanFifineSafe(IPropertyStore *store)
{
    if (!store)
        return 0;
    __try {
        return StoreMentionsFifine(store);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

class VcProxy final : public IAudioProcessingObject, public IAudioProcessingObjectRT, public IAudioProcessingObjectConfiguration, public IAudioSystemEffects3, public IAudioProcessingObjectNotifications
{
public:
    VcProxy(IUnknown *controller) : refs(0), fifine(false), useLocal(false), dryChannels(1), outer(controller), inner(NULL), innerRt(NULL), innerCfg(NULL), local(NULL), module(NULL)
    {
        identity.self = this;
        local = new (std::nothrow) MikrafApo();
        ApoTrace(controller ? L"proxy aggregated" : L"proxy alone");
    }

    ~VcProxy()
    {
        if (local)
            local->Release();
        if (innerRt)
            innerRt->Release();
        if (innerCfg)
            innerCfg->Release();
        if (inner)
            inner->Release();
        if (module)
            FreeLibrary(module);
    }

    class Identity final : public IUnknown
    {
    public:
        VcProxy *self;
        STDMETHOD(QueryInterface)(REFIID riid, void **ppv) { return self->OwnQuery(riid, ppv); }
        STDMETHOD_(ULONG, AddRef)() { return (ULONG)InterlockedIncrement(&self->refs); }
        STDMETHOD_(ULONG, Release)()
        {
            ULONG left = (ULONG)InterlockedDecrement(&self->refs);
            if (left == 0)
                delete self;
            return left;
        }
    };

    Identity identity;

    IUnknown *Controlling()
    {
        return outer ? outer : static_cast<IUnknown *>(&identity);
    }

    HRESULT OwnQuery(REFIID riid, void **ppv)
    {
        wchar_t line[64];
        if (!ppv)
            return E_POINTER;
        StringCchPrintfW(line, 64, L"qi %08lX", (unsigned long)riid.Data1);
        ApoTrace(line);
        if (riid == __uuidof(IUnknown))
            *ppv = static_cast<IUnknown *>(&identity);
        else if (riid == __uuidof(IAudioProcessingObject))
            *ppv = static_cast<IAudioProcessingObject *>(this);
        else if (riid == __uuidof(IAudioProcessingObjectRT))
            *ppv = static_cast<IAudioProcessingObjectRT *>(this);
        else if (riid == __uuidof(IAudioProcessingObjectConfiguration))
            *ppv = static_cast<IAudioProcessingObjectConfiguration *>(this);
        else if (riid == __uuidof(IAudioSystemEffects) || riid == __uuidof(IAudioSystemEffects2) || riid == __uuidof(IAudioSystemEffects3))
            *ppv = static_cast<IAudioSystemEffects3 *>(this);
        else if (riid == __uuidof(IAudioProcessingObjectNotifications))
            *ppv = static_cast<IAudioProcessingObjectNotifications *>(this);
        else {
            *ppv = NULL;
            ApoTrace(L"qi miss");
            return E_NOINTERFACE;
        }
        InterlockedIncrement(&refs);
        return S_OK;
    }

    STDMETHOD(QueryInterface)(REFIID riid, void **ppv) { return Controlling()->QueryInterface(riid, ppv); }
    STDMETHOD_(ULONG, AddRef)() { return Controlling()->AddRef(); }
    STDMETHOD_(ULONG, Release)() { return Controlling()->Release(); }

    STDMETHOD(Reset)()
    {
        ApoTrace(L"reset");
        if (inner)
            inner->Reset();
        if (local)
            local->Reset();
        return S_OK;
    }

    STDMETHOD(GetLatency)(HNSTIME *time)
    {
        ApoTrace(L"latency");
        if (useLocal && local)
            return local->GetLatency(time);
        if (inner)
            return inner->GetLatency(time);
        if (!time)
            return E_POINTER;
        *time = 0;
        return S_OK;
    }

    STDMETHOD(GetRegistrationProperties)(APO_REG_PROPERTIES **props)
    {
        APO_REG_PROPERTIES *value;
        if (!props)
            return E_POINTER;
        ApoTrace(L"registration");
        if (inner)
            return inner->GetRegistrationProperties(props);
        value = (APO_REG_PROPERTIES *)CoTaskMemAlloc(sizeof(APO_REG_PROPERTIES));
        if (!value)
            return E_OUTOFMEMORY;
        ZeroMemory(value, sizeof(*value));
        value->clsid = CLSID_VoiceClarity;
        value->Flags = (APO_FLAG)0x0c;
        StringCchCopyW(value->szFriendlyName, 256, L"Mikraf");
        StringCchCopyW(value->szCopyrightInfo, 256, L"Mikraf");
        value->u32MajorVersion = 1;
        value->u32MinorVersion = 1;
        value->u32MinInputConnections = 1;
        value->u32MaxInputConnections = 1;
        value->u32MinOutputConnections = 1;
        value->u32MaxOutputConnections = 1;
        value->u32MaxInstances = 0xffffffffu;
        value->u32NumAPOInterfaces = 1;
        value->iidAPOInterfaceList[0] = __uuidof(IAudioProcessingObject);
        *props = value;
        return S_OK;
    }

    STDMETHOD(Initialize)(UINT32 cbDataSize, BYTE *data)
    {
        HRESULT hr = S_OK;
        fifine = false;
        useLocal = false;
        ApoTrace(L"initialize");
        if (inner)
            hr = inner->Initialize(cbDataSize, data);
        if (SUCCEEDED(hr) && data && cbDataSize >= sizeof(APOInitSystemEffects)) {
            APOInitSystemEffects *fx = (APOInitSystemEffects *)data;
            fifine = ScanFifineSafe(fx->pAPOEndpointProperties) || ScanFifineSafe(fx->pAPOSystemEffectsProperties);
        }
        if (fifine && local && FAILED(local->Initialize(cbDataSize, data)))
            fifine = false;
        ApoTrace(fifine ? L"proxy fifine" : L"proxy forward");
        return hr;
    }

    STDMETHOD(IsInputFormatSupported)(IAudioMediaType *opposite, IAudioMediaType *requested, IAudioMediaType **supported)
    {
        ApoTrace(L"format in");
        if (inner)
            return inner->IsInputFormatSupported(opposite, requested, supported);
        if (local)
            return local->IsInputFormatSupported(opposite, requested, supported);
        return E_FAIL;
    }

    STDMETHOD(IsOutputFormatSupported)(IAudioMediaType *opposite, IAudioMediaType *requested, IAudioMediaType **supported)
    {
        ApoTrace(L"format out");
        if (inner)
            return inner->IsOutputFormatSupported(opposite, requested, supported);
        if (local)
            return local->IsOutputFormatSupported(opposite, requested, supported);
        return E_FAIL;
    }

    STDMETHOD(GetInputChannelCount)(UINT32 *count)
    {
        ApoTrace(L"channels");
        if (!count)
            return E_POINTER;
        if (useLocal && local)
            return local->GetInputChannelCount(count);
        if (inner)
            return inner->GetInputChannelCount(count);
        *count = dryChannels > 0 ? (UINT32)dryChannels : 1u;
        return S_OK;
    }

    STDMETHOD_(void, APOProcess)(UINT32 inputCount, APO_CONNECTION_PROPERTY **inputs, UINT32 outputCount, APO_CONNECTION_PROPERTY **outputs)
    {
        APO_CONNECTION_PROPERTY *inConn;
        APO_CONNECTION_PROPERTY *outConn;
        if (useLocal && local) {
            local->APOProcess(inputCount, inputs, outputCount, outputs);
            return;
        }
        if (innerRt) {
            innerRt->APOProcess(inputCount, inputs, outputCount, outputs);
            return;
        }
        if (inputCount < 1 || outputCount < 1 || !inputs || !outputs || !inputs[0] || !outputs[0])
            return;
        inConn = inputs[0];
        outConn = outputs[0];
        outConn->u32ValidFrameCount = inConn->u32ValidFrameCount;
        outConn->u32BufferFlags = inConn->u32BufferFlags;
        outConn->u32Signature = APO_CONNECTION_PROPERTY_SIGNATURE;
        if (inConn->u32BufferFlags != BUFFER_SILENT && inConn->pBuffer && outConn->pBuffer && dryChannels > 0)
            memcpy((void *)outConn->pBuffer, (const void *)inConn->pBuffer, sizeof(float) * inConn->u32ValidFrameCount * (size_t)dryChannels);
    }

    STDMETHOD_(UINT32, CalcInputFrames)(UINT32 outputFrames)
    {
        if (useLocal && local)
            return local->CalcInputFrames(outputFrames);
        if (innerRt)
            return innerRt->CalcInputFrames(outputFrames);
        return outputFrames;
    }

    STDMETHOD_(UINT32, CalcOutputFrames)(UINT32 inputFrames)
    {
        if (useLocal && local)
            return local->CalcOutputFrames(inputFrames);
        if (innerRt)
            return innerRt->CalcOutputFrames(inputFrames);
        return inputFrames;
    }

    STDMETHOD(LockForProcess)(UINT32 inputCount, APO_CONNECTION_DESCRIPTOR **inputs, UINT32 outputCount, APO_CONNECTION_DESCRIPTOR **outputs)
    {
        UNCOMPRESSEDAUDIOFORMAT format;
        useLocal = false;
        dryChannels = 1;
        if (fifine && local && SUCCEEDED(local->LockForProcess(inputCount, inputs, outputCount, outputs))) {
            useLocal = true;
            ApoTrace(L"lock local");
            return S_OK;
        }
        if (inputs && inputCount >= 1 && inputs[0] && inputs[0]->pFormat && SUCCEEDED(inputs[0]->pFormat->GetUncompressedAudioFormat(&format)))
            dryChannels = (int)format.dwSamplesPerFrame;
        if (dryChannels <= 0)
            dryChannels = 1;
        ApoTrace(L"lock dry");
        return S_OK;
    }

    STDMETHOD(UnlockForProcess)()
    {
        HRESULT hr = S_OK;
        if (useLocal && local)
            hr = local->UnlockForProcess();
        else if (innerCfg)
            hr = innerCfg->UnlockForProcess();
        useLocal = false;
        return hr;
    }

    STDMETHOD(GetEffectsList)(LPGUID *ids, UINT *count, HANDLE event)
    {
        ApoTrace(L"effects");
        if (inner) {
            IAudioSystemEffects2 *fx = NULL;
            if (SUCCEEDED(inner->QueryInterface(__uuidof(IAudioSystemEffects2), (void **)&fx)) && fx) {
                HRESULT hr = fx->GetEffectsList(ids, count, event);
                fx->Release();
                return hr;
            }
        }
        if (!ids || !count)
            return E_POINTER;
        *ids = (LPGUID)CoTaskMemAlloc(sizeof(GUID));
        if (*ids)
            ZeroMemory(*ids, sizeof(GUID));
        *count = 0;
        return S_OK;
    }

    STDMETHOD(GetControllableSystemEffectsList)(AUDIO_SYSTEMEFFECT **effects, UINT *numEffects, HANDLE event)
    {
        UNREFERENCED_PARAMETER(event);
        ApoTrace(L"controllable");
        if (!effects || !numEffects)
            return E_POINTER;
        *effects = (AUDIO_SYSTEMEFFECT *)CoTaskMemAlloc(sizeof(AUDIO_SYSTEMEFFECT));
        if (*effects)
            ZeroMemory(*effects, sizeof(AUDIO_SYSTEMEFFECT));
        *numEffects = 0;
        return S_OK;
    }

    STDMETHOD(SetAudioSystemEffectState)(GUID effectId, AUDIO_SYSTEMEFFECT_STATE state)
    {
        UNREFERENCED_PARAMETER(effectId);
        UNREFERENCED_PARAMETER(state);
        ApoTrace(L"set state");
        return S_OK;
    }

    STDMETHOD(GetApoNotificationRegistrationInfo)(APO_NOTIFICATION_DESCRIPTOR **info, DWORD *count)
    {
        ApoTrace(L"notify info");
        if (!info || !count)
            return E_POINTER;
        *info = NULL;
        *count = 0;
        return S_OK;
    }

    STDMETHOD_(void, HandleNotification)(APO_NOTIFICATION *notification)
    {
        UNREFERENCED_PARAMETER(notification);
    }

private:
    volatile long refs;
    bool fifine;
    bool useLocal;
    int dryChannels;
    IUnknown *outer;
    IAudioProcessingObject *inner;
    IAudioProcessingObjectRT *innerRt;
    IAudioProcessingObjectConfiguration *innerCfg;
    MikrafApo *local;
    HMODULE module;
};

class VcFactory final : public IClassFactory
{
public:
    STDMETHOD(QueryInterface)(REFIID riid, void **ppv)
    {
        if (!ppv)
            return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IClassFactory)) {
            *ppv = static_cast<IClassFactory *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() { return 2; }
    STDMETHOD_(ULONG, Release)() { return 1; }
    STDMETHOD(CreateInstance)(IUnknown *outer, REFIID riid, void **ppv)
    {
        VcProxy *proxy;
        HRESULT hr;
        if (outer && riid != __uuidof(IUnknown))
            return CLASS_E_NOAGGREGATION;
        if (!ppv)
            return E_POINTER;
        proxy = new (std::nothrow) VcProxy(outer);
        if (!proxy)
            return E_OUTOFMEMORY;
        hr = proxy->OwnQuery(riid, ppv);
        if (FAILED(hr)) {
            delete proxy;
            ApoTrace(L"create failed");
            return hr;
        }
        ApoTrace(L"created");
        return hr;
    }
    STDMETHOD(LockServer)(BOOL lock) { UNREFERENCED_PARAMETER(lock); return S_OK; }
};

static VcFactory g_vcFactory;

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved)
{
    UNREFERENCED_PARAMETER(reserved);
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}

__declspec(dllexport) STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void **ppv)
{
    wchar_t line[96];
    StringCchPrintfW(line, 96, L"DllGetClassObject %08lX", (unsigned long)clsid.Data1);
    ApoTrace(line);
    if (clsid == CLSID_VoiceClarity)
        return g_vcFactory.QueryInterface(riid, ppv);
    if (clsid != CLSID_MikrafApo)
        return CLASS_E_CLASSNOTAVAILABLE;
    return g_factory.QueryInterface(riid, ppv);
}

__declspec(dllexport) STDAPI DllCanUnloadNow(void) { return S_FALSE; }

static void WriteText(const wchar_t *path, const wchar_t *text)
{
    FILE *file = NULL;
    int bytes;
    char *utf = NULL;
    CreateDirectoryW(kDir, NULL);
    bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
    if (bytes <= 0)
        return;
    utf = (char *)malloc((size_t)bytes);
    if (!utf)
        return;
    WideCharToMultiByte(CP_UTF8, 0, text, -1, utf, bytes, NULL, NULL);
    if (_wfopen_s(&file, path, L"wb") == 0 && file) {
        fwrite(utf, 1, (size_t)bytes - 1, file);
        fclose(file);
    }
    free(utf);
}

static void LogLine(const wchar_t *text)
{
    FILE *file = NULL;
    int bytes;
    char *utf = NULL;
    CreateDirectoryW(kDir, NULL);
    bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
    if (bytes <= 1)
        return;
    utf = (char *)malloc((size_t)bytes + 2);
    if (!utf)
        return;
    WideCharToMultiByte(CP_UTF8, 0, text, -1, utf, bytes, NULL, NULL);
    utf[bytes - 1] = '\n';
    utf[bytes] = 0;
    if (_wfopen_s(&file, kLogPath, L"ab") == 0 && file) {
        fwrite(utf, 1, (size_t)bytes, file);
        fclose(file);
    }
    free(utf);
}

static void ApoTrace(const wchar_t *text)
{
    FILE *file = NULL;
    int bytes;
    char utf[256];
    bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, utf, 250, NULL, NULL);
    if (bytes <= 1)
        return;
    utf[bytes - 1] = '\n';
    if (_wfopen_s(&file, L"C:\\ProgramData\\Mikraf\\apo-load.log", L"ab") == 0 && file) {
        fwrite(utf, 1, (size_t)bytes, file);
        fclose(file);
    }
}

static void LogHr(const wchar_t *label, HRESULT hr)
{
    wchar_t line[256];
    StringCchPrintfW(line, 256, L"%s 0x%08X", label, (unsigned)hr);
    LogLine(line);
}

static bool ControlServiceWait(const wchar_t *name, bool start)
{
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    SC_HANDLE svc;
    SERVICE_STATUS_PROCESS state;
    DWORD bytes = 0;
    bool ok = false;
    if (!scm)
        return false;
    svc = OpenServiceW(scm, name, SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!svc) {
        CloseServiceHandle(scm);
        return false;
    }
    if (!start)
        ControlService(svc, SERVICE_CONTROL_STOP, (LPSERVICE_STATUS)&state);
    for (int attempt = 0; attempt < (start ? 4 : 1) && !ok; attempt++) {
        if (start)
            StartServiceW(svc, 0, NULL);
        for (int i = 0; i < 80; i++) {
            if (!QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, (LPBYTE)&state, sizeof(state), &bytes))
                break;
            if (state.dwCurrentState == (start ? SERVICE_RUNNING : SERVICE_STOPPED)) {
                ok = true;
                break;
            }
            if (start && state.dwCurrentState == SERVICE_STOPPED)
                break;
            Sleep(100);
        }
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok;
}

static void RestartAudio()
{
    ControlServiceWait(L"AudioEndpointBuilder", false);
    ControlServiceWait(L"Audiosrv", false);
    ControlServiceWait(L"Audiosrv", true);
    ControlServiceWait(L"AudioEndpointBuilder", true);
}

static const wchar_t *kFxNames[] = {
    L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},4",
    L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5",
    L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},13",
    L"{d3993a3f-99c2-4402-b5ec-a92a0367664b},5",
    L"{9e6136e0-57ab-4949-b57a-3627be142855},100",
};
static const wchar_t *kPropNames[] = {
    L"{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},5",
    L"{c876062a-a276-4ed6-b15b-3962e69007f8},2",
    L"{c876062a-a276-4ed6-b15b-3962e69007f8},3",
};

static void BackupValues(const wchar_t *guid, const wchar_t *deviceId)
{
    FILE *file = NULL;
    uint32_t had = 0;
    uint32_t value = 0;
    DWORD cb = sizeof(value);
    HKEY audio = NULL;
    wchar_t fxPath[320];
    wchar_t propPath[320];
    HKEY fx = NULL;
    HKEY prop = NULL;
    if (_wfopen_s(&file, kBackup, L"wb") != 0 || !file)
        return;
    fwrite("MIKRBK01", 1, 8, file);
    fwrite(deviceId, sizeof(wchar_t), 260, file);
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Audio", 0, KEY_READ | KEY_WOW64_64KEY, &audio) == ERROR_SUCCESS) {
        if (RegQueryValueExW(audio, L"DisableProtectedAudioDG", NULL, NULL, (LPBYTE)&value, &cb) == ERROR_SUCCESS && cb == sizeof(value))
            had = 1;
        RegCloseKey(audio);
    }
    fwrite(&had, 4, 1, file);
    fwrite(&value, 4, 1, file);
    StringCchPrintfW(fxPath, 320, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Capture\\%s\\FxProperties", guid);
    StringCchPrintfW(propPath, 320, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Capture\\%s\\Properties", guid);
    RegOpenKeyExW(HKEY_LOCAL_MACHINE, fxPath, 0, KEY_READ | KEY_WOW64_64KEY, &fx);
    RegOpenKeyExW(HKEY_LOCAL_MACHINE, propPath, 0, KEY_READ | KEY_WOW64_64KEY, &prop);
    for (int hive = 0; hive < 2; hive++) {
        const wchar_t **names = hive == 0 ? kFxNames : kPropNames;
        int count = hive == 0 ? 5 : 3;
        HKEY key = hive == 0 ? fx : prop;
        for (int i = 0; i < count; i++) {
            BYTE stack[8];
            DWORD type = 0;
            DWORD size = 0;
            uint32_t present = 0;
            LONG rc = key ? RegQueryValueExW(key, names[i], NULL, &type, NULL, &size) : ERROR_FILE_NOT_FOUND;
            BYTE *data = NULL;
            fwrite(&hive, 4, 1, file);
            fwrite(names[i], sizeof(wchar_t), 128, file);
            if (rc == ERROR_SUCCESS) {
                present = 1;
                data = (BYTE *)malloc(size ? size : 1);
                if (data && RegQueryValueExW(key, names[i], NULL, &type, data, &size) != ERROR_SUCCESS)
                    present = 0;
            }
            fwrite(&present, 4, 1, file);
            fwrite(&type, 4, 1, file);
            fwrite(&size, 4, 1, file);
            if (present && data)
                fwrite(data, 1, size, file);
            else
                fwrite(stack, 1, 0, file);
            free(data);
        }
    }
    if (fx)
        RegCloseKey(fx);
    if (prop)
        RegCloseKey(prop);
    fclose(file);
}

static int RestoreBackup()
{
    FILE *file = NULL;
    char magic[8];
    wchar_t deviceId[260];
    uint32_t had = 0;
    uint32_t value = 0;
    wchar_t guid[80];
    HKEY audio = NULL;
    if (_wfopen_s(&file, kBackup, L"rb") != 0 || !file)
        return 0;
    if (fread(magic, 1, 8, file) != 8 || memcmp(magic, "MIKRBK01", 8) != 0) {
        fclose(file);
        return 0;
    }
    if (fread(deviceId, sizeof(wchar_t), 260, file) != 260 || fread(&had, 4, 1, file) != 1 || fread(&value, 4, 1, file) != 1) {
        fclose(file);
        return 0;
    }
    deviceId[259] = 0;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Audio", 0, NULL, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, NULL, &audio, NULL) == ERROR_SUCCESS) {
        if (had)
            RegSetValueExW(audio, L"DisableProtectedAudioDG", 0, REG_DWORD, (BYTE *)&value, sizeof(value));
        else
            RegDeleteValueW(audio, L"DisableProtectedAudioDG");
        RegCloseKey(audio);
    }
    if (!EndpointGuid(deviceId, guid, 80)) {
        fclose(file);
        return 0;
    }
    for (int n = 0; n < 16; n++) {
        uint32_t hive = 0, present = 0, type = 0, size = 0;
        wchar_t name[128];
        BYTE *data = NULL;
        wchar_t path[360];
        HKEY key = NULL;
        if (fread(&hive, 4, 1, file) != 1 || fread(name, sizeof(wchar_t), 128, file) != 128)
            break;
        name[127] = 0;
        if (fread(&present, 4, 1, file) != 1 || fread(&type, 4, 1, file) != 1 || fread(&size, 4, 1, file) != 1)
            break;
        if (size > 0 && size < 1u << 20) {
            data = (BYTE *)malloc(size);
            if (!data || fread(data, 1, size, file) != size) {
                free(data);
                break;
            }
        }
        StringCchPrintfW(path, 360, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Capture\\%s\\%s", guid, hive == 0 ? L"FxProperties" : L"Properties");
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS) {
            if (present && data)
                RegSetValueExW(key, name, 0, type, data, size);
            else
                RegDeleteValueW(key, name);
            RegCloseKey(key);
        }
        free(data);
    }
    fclose(file);
    return 1;
}

static HRESULT RegisterCom(const wchar_t *dllPath)
{
    wchar_t keyPath[256];
    HKEY key = NULL;
    HRESULT hr;
    StringCchPrintfW(keyPath, 256, L"SOFTWARE\\Classes\\CLSID\\%s", kClsid);
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, keyPath, 0, NULL, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, NULL, &key, NULL) != ERROR_SUCCESS)
        return E_FAIL;
    RegSetValueExW(key, NULL, 0, REG_SZ, (BYTE *)L"Mikraf", sizeof(L"Mikraf"));
    RegCloseKey(key);
    StringCchPrintfW(keyPath, 256, L"SOFTWARE\\Classes\\CLSID\\%s\\InprocServer32", kClsid);
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, keyPath, 0, NULL, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, NULL, &key, NULL) != ERROR_SUCCESS)
        return E_FAIL;
    RegSetValueExW(key, NULL, 0, REG_SZ, (BYTE *)dllPath, (DWORD)((wcslen(dllPath) + 1) * sizeof(wchar_t)));
    RegSetValueExW(key, L"ThreadingModel", 0, REG_SZ, (BYTE *)L"Both", sizeof(L"Both"));
    RegCloseKey(key);
    StringCchPrintfW(keyPath, 256, L"SOFTWARE\\Classes\\AudioEngine\\AudioProcessingObjects\\%s", kClsid);
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, keyPath, 0, NULL, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, NULL, &key, NULL) != ERROR_SUCCESS)
        return E_FAIL;
    {
        DWORD one = 1;
        DWORD flags = APO_FLAG_NONE;
        DWORD maxInstances = 0xffffffffu;
        wchar_t iid[80];
        StringFromGUID2(__uuidof(IAudioSystemEffects), iid, 80);
        RegSetValueExW(key, L"FriendlyName", 0, REG_SZ, (BYTE *)L"Mikraf", sizeof(L"Mikraf"));
        RegSetValueExW(key, L"Copyright", 0, REG_SZ, (BYTE *)L"Mikraf", sizeof(L"Mikraf"));
        RegSetValueExW(key, L"MajorVersion", 0, REG_DWORD, (BYTE *)&one, sizeof(one));
        one = 0;
        RegSetValueExW(key, L"MinorVersion", 0, REG_DWORD, (BYTE *)&one, sizeof(one));
        RegSetValueExW(key, L"Flags", 0, REG_DWORD, (BYTE *)&flags, sizeof(flags));
        one = 1;
        RegSetValueExW(key, L"MinInputConnections", 0, REG_DWORD, (BYTE *)&one, sizeof(one));
        RegSetValueExW(key, L"MaxInputConnections", 0, REG_DWORD, (BYTE *)&one, sizeof(one));
        RegSetValueExW(key, L"MinOutputConnections", 0, REG_DWORD, (BYTE *)&one, sizeof(one));
        RegSetValueExW(key, L"MaxOutputConnections", 0, REG_DWORD, (BYTE *)&one, sizeof(one));
        RegSetValueExW(key, L"MaxInstances", 0, REG_DWORD, (BYTE *)&maxInstances, sizeof(maxInstances));
        one = 2;
        RegSetValueExW(key, L"NumAPOInterfaces", 0, REG_DWORD, (BYTE *)&one, sizeof(one));
        StringFromGUID2(__uuidof(IAudioProcessingObject), iid, 80);
        RegSetValueExW(key, L"APOInterface0", 0, REG_SZ, (BYTE *)iid, (DWORD)((wcslen(iid) + 1) * sizeof(wchar_t)));
        StringFromGUID2(__uuidof(IAudioSystemEffects), iid, 80);
        RegSetValueExW(key, L"APOInterface1", 0, REG_SZ, (BYTE *)iid, (DWORD)((wcslen(iid) + 1) * sizeof(wchar_t)));
    }
    RegCloseKey(key);
    hr = S_OK;
    return hr;
}

static void UnregisterCom()
{
    wchar_t keyPath[256];
    StringCchPrintfW(keyPath, 256, L"SOFTWARE\\Classes\\CLSID\\%s", kClsid);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, keyPath);
    StringCchPrintfW(keyPath, 256, L"SOFTWARE\\Classes\\AudioEngine\\AudioProcessingObjects\\%s", kClsid);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, keyPath);
}

static int WaitForCaptureDevice(const wchar_t *deviceId)
{
    for (int i = 0; i < 40; i++) {
        IMMDeviceEnumerator *enumerator = NULL;
        IMMDevice *device = NULL;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void **)&enumerator);
        if (SUCCEEDED(hr) && enumerator) {
            hr = enumerator->GetDevice(deviceId, &device);
            if (device)
                device->Release();
            enumerator->Release();
            if (SUCCEEDED(hr))
                return 1;
        }
        Sleep(250);
    }
    return 0;
}

static HRESULT WriteFxRegistry(const wchar_t *deviceId);

static HRESULT AttachEndpoint(const wchar_t *deviceId)
{
    IMMDeviceEnumerator *enumerator = NULL;
    IMMDevice *device = NULL;
    IPropertyStore *store = NULL;
    HRESULT hr;
    PROPVARIANT value;
    const wchar_t *modes[4] = {
        L"{C18E2F7E-933D-4965-B7D1-1EEF228D2AF3}",
        L"{9E90EA20-B493-4FD1-A1A8-7E1361A956CF}",
        L"{98951333-B9CD-48B1-A0A3-FF40682D73F7}",
        L"{FC1CFC9B-B9D6-4CFA-B5E0-4BB2166878B2}",
    };
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void **)&enumerator);
    if (FAILED(hr))
        return hr;
    hr = enumerator->GetDevice(deviceId, &device);
    if (FAILED(hr)) {
        enumerator->Release();
        return hr;
    }
    hr = device->OpenPropertyStore(STGM_READWRITE, &store);
    if (FAILED(hr)) {
        device->Release();
        enumerator->Release();
        return hr;
    }
    hr = InitPropVariantFromString(kClsid, &value);
    if (SUCCEEDED(hr))
        hr = store->SetValue(kFxStream, value);
    PropVariantClear(&value);
    if (SUCCEEDED(hr)) {
        hr = InitPropVariantFromString(kClsid, &value);
        if (SUCCEEDED(hr))
            hr = store->SetValue(kFxCompositeStream, value);
        PropVariantClear(&value);
    }
    if (SUCCEEDED(hr)) {
        hr = InitPropVariantFromString(L"Mikraf", &value);
        if (SUCCEEDED(hr))
            hr = store->SetValue(kFxFriendly, value);
        PropVariantClear(&value);
    }
    if (SUCCEEDED(hr)) {
        hr = InitPropVariantFromStringVector(modes, 4, &value);
        if (SUCCEEDED(hr))
            hr = store->SetValue(kSfxModes, value);
        PropVariantClear(&value);
    }
    if (SUCCEEDED(hr)) {
        PropVariantInit(&value);
        value.vt = VT_UI4;
        value.ulVal = 0;
        hr = store->SetValue(kDisableSysFx, value);
        PropVariantClear(&value);
    }
    if (SUCCEEDED(hr))
        hr = store->Commit();
    store->Release();
    device->Release();
    enumerator->Release();
    if (SUCCEEDED(hr))
        hr = WriteFxRegistry(deviceId);
    return hr;
}

static HRESULT WriteFxRegistry(const wchar_t *deviceId)
{
    wchar_t guid[80];
    wchar_t path[360];
    HKEY key = NULL;
    DWORD enabled = 0;
    wchar_t modes[512];
    const wchar_t *modeList[] = {
        L"{C18E2F7E-933D-4965-B7D1-1EEF228D2AF3}",
        L"{9E90EA20-B493-4FD1-A1A8-7E1361A956CF}",
        L"{98951333-B9CD-48B1-A0A3-FF40682D73F7}",
        L"{FC1CFC9B-B9D6-4CFA-B5E0-4BB2166878B2}",
    };
    int used = 0;
    if (!EndpointGuid(deviceId, guid, 80))
        return E_INVALIDARG;
    StringCchPrintfW(path, 360, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Capture\\%s\\FxProperties", guid);
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, path, 0, NULL, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, NULL, &key, NULL) != ERROR_SUCCESS)
        return E_FAIL;
    RegSetValueExW(key, L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5", 0, REG_SZ, (BYTE *)kClsid, (DWORD)((wcslen(kClsid) + 1) * sizeof(wchar_t)));
    RegSetValueExW(key, L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},13", 0, REG_SZ, (BYTE *)kClsid, (DWORD)((wcslen(kClsid) + 1) * sizeof(wchar_t)));
    RegSetValueExW(key, L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},4", 0, REG_SZ, (BYTE *)L"Mikraf", sizeof(L"Mikraf"));
    RegSetValueExW(key, L"{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},5", 0, REG_DWORD, (BYTE *)&enabled, sizeof(enabled));
    ZeroMemory(modes, sizeof(modes));
    for (int i = 0; i < 4; i++) {
        if (used + (int)wcslen(modeList[i]) + 2 >= 512)
            break;
        wcscpy_s(modes + used, 512 - used, modeList[i]);
        used += (int)wcslen(modeList[i]) + 1;
    }
    modes[used] = 0;
    RegSetValueExW(key, L"{d3993a3f-99c2-4402-b5ec-a92a0367664b},5", 0, REG_MULTI_SZ, (BYTE *)modes, (DWORD)((used + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return S_OK;
}

// Voice Clarity's effect pack replaces SFX on every real capture endpoint.
// Drop that binding for this microphone only. Remove restores vc-backup.bin.
static void WriteVoiceRecord(FILE *file, HKEY key, uint32_t hive, const wchar_t *name)
{
    BYTE stack[4];
    DWORD type = 0;
    DWORD size = 0;
    uint32_t present = 0;
    wchar_t nameBuf[128];
    LONG rc = key ? RegQueryValueExW(key, name, NULL, &type, NULL, &size) : ERROR_FILE_NOT_FOUND;
    BYTE *data = NULL;
    ZeroMemory(nameBuf, sizeof(nameBuf));
    StringCchCopyW(nameBuf, 128, name);
    fwrite(&hive, 4, 1, file);
    fwrite(nameBuf, sizeof(wchar_t), 128, file);
    if (rc == ERROR_SUCCESS) {
        present = 1;
        data = (BYTE *)malloc(size ? size : 1);
        if (data && RegQueryValueExW(key, name, NULL, &type, data, &size) != ERROR_SUCCESS)
            present = 0;
    }
    fwrite(&present, 4, 1, file);
    fwrite(&type, 4, 1, file);
    fwrite(&size, 4, 1, file);
    if (present && data)
        fwrite(data, 1, size, file);
    else
        fwrite(stack, 1, 0, file);
    free(data);
}

static void SaveVoiceClarity(const wchar_t *guid)
{
    FILE *file = NULL;
    HKEY fx = NULL;
    HKEY prop = NULL;
    wchar_t path[360];
    if (GetFileAttributesW(kVoiceBackup) != INVALID_FILE_ATTRIBUTES)
        return;
    if (_wfopen_s(&file, kVoiceBackup, L"wb") != 0 || !file)
        return;
    fwrite("MIKRVC01", 1, 8, file);
    StringCchPrintfW(path, 360, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Capture\\%s\\FxProperties", guid);
    RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_READ | KEY_WOW64_64KEY, &fx);
    StringCchPrintfW(path, 360, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Capture\\%s\\Properties", guid);
    RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_READ | KEY_WOW64_64KEY, &prop);
    WriteVoiceRecord(file, fx, 0, L"{9e6136e0-57ab-4949-b57a-3627be142855},100");
    WriteVoiceRecord(file, prop, 1, L"{c876062a-a276-4ed6-b15b-3962e69007f8},2");
    WriteVoiceRecord(file, prop, 1, L"{c876062a-a276-4ed6-b15b-3962e69007f8},3");
    if (fx)
        RegCloseKey(fx);
    if (prop)
        RegCloseKey(prop);
    fclose(file);
}

static void RestoreVoiceClarity(const wchar_t *guid)
{
    FILE *file = NULL;
    char magic[8];
    if (_wfopen_s(&file, kVoiceBackup, L"rb") != 0 || !file)
        return;
    if (fread(magic, 1, 8, file) != 8 || memcmp(magic, "MIKRVC01", 8) != 0) {
        fclose(file);
        return;
    }
    for (int n = 0; n < 3; n++) {
        uint32_t hive = 0, present = 0, type = 0, size = 0;
        wchar_t name[128];
        BYTE *data = NULL;
        wchar_t path[360];
        HKEY key = NULL;
        if (fread(&hive, 4, 1, file) != 1 || fread(name, sizeof(wchar_t), 128, file) != 128)
            break;
        name[127] = 0;
        if (fread(&present, 4, 1, file) != 1 || fread(&type, 4, 1, file) != 1 || fread(&size, 4, 1, file) != 1)
            break;
        if (size > 0 && size < 1u << 20) {
            data = (BYTE *)malloc(size);
            if (!data || fread(data, 1, size, file) != size) {
                free(data);
                break;
            }
        }
        StringCchPrintfW(path, 360, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Capture\\%s\\%s", guid, hive == 0 ? L"FxProperties" : L"Properties");
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS) {
            if (present && data)
                RegSetValueExW(key, name, 0, type, data, size);
            else
                RegDeleteValueW(key, name);
            RegCloseKey(key);
        }
        free(data);
    }
    fclose(file);
    DeleteFileW(kVoiceBackup);
}

static void DetachVoiceClarity(const wchar_t *guid)
{
    wchar_t path[360];
    HKEY key = NULL;
    SaveVoiceClarity(guid);
    StringCchPrintfW(path, 360, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Capture\\%s\\FxProperties", guid);
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS) {
        RegDeleteValueW(key, L"{9e6136e0-57ab-4949-b57a-3627be142855},100");
        RegCloseKey(key);
    }
    StringCchPrintfW(path, 360, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Capture\\%s\\Properties", guid);
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS) {
        RegDeleteValueW(key, L"{c876062a-a276-4ed6-b15b-3962e69007f8},2");
        RegDeleteValueW(key, L"{c876062a-a276-4ed6-b15b-3962e69007f8},3");
        RegCloseKey(key);
    }
}

// Killing audiodg alone leaves a fresh install unloaded until the audio services restart.
static void RestartAudioServices(const wchar_t *system)
{
    wchar_t command[MAX_PATH + 160];
    STARTUPINFOW startup;
    PROCESS_INFORMATION info;
    StringCchPrintfW(command, MAX_PATH + 160, L"%s\cmd.exe /c \"net stop audiosrv /y & net stop AudioEndpointBuilder /y & net start AudioEndpointBuilder & net start audiosrv\"", system);
    ZeroMemory(&startup, sizeof(startup));
    startup.cb = sizeof(startup);
    ZeroMemory(&info, sizeof(info));
    if (CreateProcessW(NULL, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &startup, &info)) {
        WaitForSingleObject(info.hProcess, 30000);
        CloseHandle(info.hThread);
        CloseHandle(info.hProcess);
        LogLine(L"audio services restarted");
    }
}

static void RecycleAudioEngine()
{
    wchar_t system[MAX_PATH];
    wchar_t command[MAX_PATH + 32];
    STARTUPINFOW startup;
    PROCESS_INFORMATION info;
    GetSystemDirectoryW(system, MAX_PATH);
    StringCchPrintfW(command, MAX_PATH + 32, L"%s\\taskkill.exe /F /IM audiodg.exe", system);
    ZeroMemory(&startup, sizeof(startup));
    startup.cb = sizeof(startup);
    ZeroMemory(&info, sizeof(info));
    if (CreateProcessW(NULL, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &startup, &info)) {
        WaitForSingleObject(info.hProcess, 5000);
        CloseHandle(info.hThread);
        CloseHandle(info.hProcess);
        LogLine(L"audiodg recycled");
        RestartAudioServices(system);
    } else {
        LogLine(L"audiodg recycle failed");
    }
    Sleep(800);
}

static int CopyRuntime()
{
    wchar_t source[MAX_PATH];
    wchar_t *slash;
    wchar_t fromApo[MAX_PATH];
    wchar_t fromSpeex[MAX_PATH];
    wchar_t toApo[MAX_PATH];
    wchar_t toSpeex[MAX_PATH];
    if (!GetModuleFileNameW(g_module, source, MAX_PATH))
        return 0;
    slash = wcsrchr(source, L'\\');
    if (!slash)
        return 0;
    *slash = 0;
    StringCchPrintfW(fromApo, MAX_PATH, L"%s\\mikraf-apo.dll", source);
    StringCchPrintfW(fromSpeex, MAX_PATH, L"%s\\mikraf-speex.dll", source);
    CreateDirectoryW(kInstallDir, NULL);
    StringCchPrintfW(toApo, MAX_PATH, L"%s\\mikraf-apo.dll", kInstallDir);
    StringCchPrintfW(toSpeex, MAX_PATH, L"%s\\mikraf-speex.dll", kInstallDir);
    if (_wcsicmp(source, kInstallDir) == 0)
        return 1;
    if (!CopyFileW(fromApo, toApo, FALSE))
        return 0;
    if (!CopyFileW(fromSpeex, toSpeex, FALSE))
        return 0;
    return 1;
}

static void EnableProtectedBypass()
{
    HKEY audio = NULL;
    DWORD value = 1;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Audio", 0, NULL, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, NULL, &audio, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(audio, L"DisableProtectedAudioDG", 0, REG_DWORD, (BYTE *)&value, sizeof(value));
        RegCloseKey(audio);
    }
}

static const wchar_t kVcRemoved[] = L"C:\\ProgramData\\Mikraf\\vc-driver-removed.txt";
static const wchar_t kVcDriverRoot[] = L"C:\\ProgramData\\Mikraf\\vc-driver";

static int RunHidden(const wchar_t *command, const wchar_t *logPath, DWORD *exitCode)
{
    SECURITY_ATTRIBUTES security;
    HANDLE log;
    STARTUPINFOW startup;
    PROCESS_INFORMATION info;
    wchar_t mutableCommand[1200];
    DWORD code = 1;
    ZeroMemory(&security, sizeof(security));
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    log = CreateFileW(logPath, GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (log == INVALID_HANDLE_VALUE)
        return 0;
    ZeroMemory(&startup, sizeof(startup));
    ZeroMemory(&info, sizeof(info));
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdOutput = log;
    startup.hStdError = log;
    StringCchCopyW(mutableCommand, 1200, command);
    if (!CreateProcessW(NULL, mutableCommand, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &startup, &info)) {
        CloseHandle(log);
        return 0;
    }
    WaitForSingleObject(info.hProcess, 120000);
    GetExitCodeProcess(info.hProcess, &code);
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    CloseHandle(log);
    if (exitCode)
        *exitCode = code;
    return 1;
}

static wchar_t *LoadWideFile(const wchar_t *path)
{
    HANDLE file;
    DWORD size;
    DWORD read = 0;
    BYTE *bytes;
    wchar_t *text = NULL;
    int chars;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return NULL;
    size = GetFileSize(file, NULL);
    if (size == INVALID_FILE_SIZE || size > 1024 * 1024) {
        CloseHandle(file);
        return NULL;
    }
    bytes = (BYTE *)malloc((size_t)size + 2);
    if (!bytes || !ReadFile(file, bytes, size, &read, NULL)) {
        free(bytes);
        CloseHandle(file);
        return NULL;
    }
    CloseHandle(file);
    bytes[read] = 0;
    bytes[read + 1] = 0;
    if (read >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
        chars = (int)((read - 2) / sizeof(wchar_t));
        text = (wchar_t *)malloc(((size_t)chars + 1) * sizeof(wchar_t));
        if (text) {
            memcpy(text, bytes + 2, (size_t)chars * sizeof(wchar_t));
            text[chars] = 0;
        }
    } else {
        UINT codePage = CP_UTF8;
        DWORD flags = MB_ERR_INVALID_CHARS;
        chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (char *)bytes, (int)read, NULL, 0);
        if (chars <= 0) {
            codePage = CP_ACP;
            flags = 0;
            chars = MultiByteToWideChar(CP_ACP, 0, (char *)bytes, (int)read, NULL, 0);
        }
        if (chars > 0) {
            text = (wchar_t *)malloc(((size_t)chars + 1) * sizeof(wchar_t));
            if (text) {
                MultiByteToWideChar(codePage, flags, (char *)bytes, (int)read, text, chars);
                text[chars] = 0;
            }
        }
    }
    free(bytes);
    return text;
}

static void TrimLine(wchar_t *line)
{
    wchar_t *start = line;
    size_t length;
    while (*start == L' ' || *start == L'\t')
        start++;
    if (start != line)
        memmove(line, start, (wcslen(start) + 1) * sizeof(wchar_t));
    length = wcslen(line);
    while (length > 0 && (line[length - 1] == L'\r' || line[length - 1] == L' ' || line[length - 1] == L'\t'))
        line[--length] = 0;
}

static int ContainsFold(const wchar_t *hay, const wchar_t *needle)
{
    size_t n;
    if (!hay || !needle || !needle[0])
        return 0;
    n = wcslen(needle);
    for (; *hay; hay++) {
        if (_wcsnicmp(hay, needle, n) == 0)
            return 1;
    }
    return 0;
}

// oem12.inf, independent of the pnputil column language.
static int CopyOemName(const wchar_t *line, wchar_t *out, int outChars)
{
    const wchar_t *p = line;
    while (*p) {
        if (_wcsnicmp(p, L"oem", 3) == 0 && iswdigit(p[3])) {
            const wchar_t *end = p + 3;
            int n;
            while (iswdigit(*end))
                end++;
            if (_wcsnicmp(end, L".inf", 4) != 0) {
                p++;
                continue;
            }
            end += 4;
            n = (int)(end - p);
            if (n + 1 > outChars)
                return 0;
            wmemcpy(out, p, (size_t)n);
            out[n] = 0;
            return 1;
        }
        p++;
    }
    return 0;
}

static int CollectVoiceInfs(wchar_t infs[][40], int capacity)
{
    wchar_t system[MAX_PATH];
    wchar_t command[MAX_PATH + 80];
    wchar_t *text;
    wchar_t *cursor;
    wchar_t published[40];
    int count = 0;
    GetSystemDirectoryW(system, MAX_PATH);
    StringCchPrintfW(command, MAX_PATH + 80, L"\"%s\\pnputil.exe\" /enum-drivers", system);
    if (!RunHidden(command, L"C:\\ProgramData\\Mikraf\\pnputil-enum.txt", NULL))
        return 0;
    text = LoadWideFile(L"C:\\ProgramData\\Mikraf\\pnputil-enum.txt");
    if (!text)
        return 0;
    published[0] = 0;
    cursor = text;
    while (*cursor && count < capacity) {
        wchar_t line[256];
        wchar_t oem[40];
        int i = 0;
        while (*cursor && *cursor != L'\n' && i < 255)
            line[i++] = *cursor++;
        line[i] = 0;
        if (*cursor == L'\n')
            cursor++;
        TrimLine(line);
        if (CopyOemName(line, oem, 40))
            StringCchCopyW(published, 40, oem);
        if (published[0] && ContainsFold(line, L"voiceclarity")) {
            StringCchCopyW(infs[count], 40, published);
            count++;
            published[0] = 0;
        }
    }
    free(text);
    return count;
}

static void BackupVoiceDrivers()
{
    WIN32_FIND_DATAW data;
    HANDLE find;
    wchar_t system[MAX_PATH];
    CreateDirectoryW(kVcDriverRoot, NULL);
    GetSystemDirectoryW(system, MAX_PATH);
    find = FindFirstFileW(L"C:\\Windows\\System32\\DriverStore\\FileRepository\\voiceclarity*", &data);
    if (find == INVALID_HANDLE_VALUE)
        return;
    do {
        wchar_t source[MAX_PATH];
        wchar_t dest[MAX_PATH];
        wchar_t command[1100];
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || data.cFileName[0] == L'.')
            continue;
        StringCchPrintfW(source, MAX_PATH, L"C:\\Windows\\System32\\DriverStore\\FileRepository\\%s", data.cFileName);
        StringCchPrintfW(dest, MAX_PATH, L"%s\\%s", kVcDriverRoot, data.cFileName);
        StringCchPrintfW(command, 1100, L"\"%s\\robocopy.exe\" \"%s\" \"%s\" /E /NFL /NDL /NJH /NJS /NP", system, source, dest);
        RunHidden(command, L"C:\\ProgramData\\Mikraf\\vc-copy.log", NULL);
    } while (FindNextFileW(find, &data));
    FindClose(find);
}

// 1: a Voice Clarity package was removed. 0: none installed. -1: removal failed.
static int PrepareVoiceClarity()
{
    wchar_t infs[8][40];
    wchar_t system[MAX_PATH];
    int count;
    int removed = 0;
    int failed = 0;
    int i;
    count = CollectVoiceInfs(infs, 8);
    if (count <= 0)
        return 0;
    BackupVoiceDrivers();
    GetSystemDirectoryW(system, MAX_PATH);
    for (i = 0; i < count; i++) {
        wchar_t command[MAX_PATH + 80];
        DWORD code = 1;
        StringCchPrintfW(command, MAX_PATH + 80, L"\"%s\\pnputil.exe\" /delete-driver %s /uninstall", system, infs[i]);
        if (!RunHidden(command, L"C:\\ProgramData\\Mikraf\\vc-delete.log", &code) || (code != 0 && code != 3010))
            failed++;
        else
            removed++;
    }
    if (failed)
        return -1;
    if (removed > 0) {
        HANDLE marker = CreateFileW(kVcRemoved, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (marker != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(marker, "removed\r\n", 9, &written, NULL);
            CloseHandle(marker);
        }
        LogLine(L"voice clarity driver removed");
        return 1;
    }
    return 0;
}

static void ClearPackLinks()
{
    HKEY capture = NULL;
    DWORD index = 0;
    const wchar_t *root = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Capture";
    const wchar_t *linkName = L"{9e6136e0-57ab-4949-b57a-3627be142855},100";
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, root, 0, KEY_READ | KEY_WOW64_64KEY, &capture) != ERROR_SUCCESS)
        return;
    for (;;) {
        wchar_t name[80];
        wchar_t path[460];
        DWORD nameLen = 80;
        DWORD type = 0;
        DWORD size = 0;
        HKEY fx = NULL;
        HKEY prop = NULL;
        if (RegEnumKeyExW(capture, index++, name, &nameLen, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        StringCchPrintfW(path, 460, L"%s\\%s\\FxProperties", root, name);
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY, &fx) != ERROR_SUCCESS)
            continue;
        size = 0;
        if (RegQueryValueExW(fx, linkName, NULL, &type, NULL, &size) == ERROR_SUCCESS || RegQueryValueExW(fx, linkName, NULL, &type, NULL, &size) == ERROR_MORE_DATA) {
            if (type == REG_SZ || type == REG_EXPAND_SZ) {
                RegDeleteValueW(fx, linkName);
                StringCchPrintfW(path, 460, L"%s\\%s\\Properties", root, name);
                if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &prop) == ERROR_SUCCESS) {
                    RegDeleteValueW(prop, L"{c876062a-a276-4ed6-b15b-3962e69007f8},2");
                    RegDeleteValueW(prop, L"{c876062a-a276-4ed6-b15b-3962e69007f8},3");
                    RegCloseKey(prop);
                }
            }
        }
        RegCloseKey(fx);
    }
    RegCloseKey(capture);
}

static void StripMikrafSlots()
{
    HKEY capture = NULL;
    DWORD index = 0;
    const wchar_t *root = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Capture";
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, root, 0, KEY_READ | KEY_WOW64_64KEY, &capture) != ERROR_SUCCESS)
        return;
    for (;;) {
        wchar_t name[80];
        wchar_t path[460];
        wchar_t value[80];
        DWORD nameLen = 80;
        DWORD size;
        HKEY fx = NULL;
        if (RegEnumKeyExW(capture, index++, name, &nameLen, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        StringCchPrintfW(path, 460, L"%s\\%s\\FxProperties", root, name);
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY, &fx) != ERROR_SUCCESS)
            continue;
        size = sizeof(value);
        if (RegQueryValueExW(fx, L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5", NULL, NULL, (BYTE *)value, &size) == ERROR_SUCCESS) {
            value[79] = 0;
            if (_wcsicmp(value, kClsid) == 0)
                RegDeleteValueW(fx, L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5");
        }
        size = sizeof(value);
        if (RegQueryValueExW(fx, L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},13", NULL, NULL, (BYTE *)value, &size) == ERROR_SUCCESS) {
            value[79] = 0;
            if (_wcsicmp(value, kClsid) == 0)
                RegDeleteValueW(fx, L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},13");
        }
        size = sizeof(value);
        if (RegQueryValueExW(fx, L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},4", NULL, NULL, (BYTE *)value, &size) == ERROR_SUCCESS) {
            value[79] = 0;
            if (_wcsicmp(value, L"Mikraf") == 0)
                RegDeleteValueW(fx, L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},4");
        }
        RegCloseKey(fx);
    }
    RegCloseKey(capture);
}

static void InstallMain(const wchar_t *deviceId)
{
    wchar_t guid[80];
    wchar_t dllPath[MAX_PATH];
    HRESULT hr;
    wchar_t line[512];
    int voice = 0;
    int comReady = 0;
    CreateDirectoryW(kDir, NULL);
    AllowFolder(kDir);
    LogLine(L"install start");
    if (!EndpointGuid(deviceId, guid, 80)) {
        WriteText(kResult, L"Mikrofon kimliği okunamadı.");
        return;
    }
    {
        wchar_t path[360];
        HKEY key = NULL;
        StringCchPrintfW(path, 360, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Capture\\%s", guid);
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
            WriteText(kResult, L"Seçilen aygıt bir kayıt mikrofonu değil.");
            return;
        }
        RegCloseKey(key);
    }
    if (GetFileAttributesW(kBackup) != INVALID_FILE_ATTRIBUTES) {
        LogLine(L"backup kept");
    } else {
        BackupValues(guid, deviceId);
        LogLine(L"backup written");
    }
    ControlServiceWait(L"AudioEndpointBuilder", false);
    ControlServiceWait(L"Audiosrv", false);
    if (!CopyRuntime()) {
        DWORD err = GetLastError();
        StringCchPrintfW(line, 512, L"DLL kopyalanamadı (%lu). mikraf-speex.dll Mikraf klasöründe olmalı.", err);
        WriteText(kResult, line);
        LogLine(line);
        ControlServiceWait(L"Audiosrv", true);
        ControlServiceWait(L"AudioEndpointBuilder", true);
        return;
    }
    StringCchPrintfW(dllPath, MAX_PATH, L"%s\\mikraf-apo.dll", kInstallDir);
    hr = RegisterCom(dllPath);
    LogHr(L"RegisterAPO", hr);
    if (FAILED(hr)) {
        WriteText(kResult, L"APO kaydı başarısız. Ses hizmeti yeniden açıldı.");
        ControlServiceWait(L"Audiosrv", true);
        ControlServiceWait(L"AudioEndpointBuilder", true);
        return;
    }
    EnableProtectedBypass();
    {
        voice = PrepareVoiceClarity();
        if (voice >= 0)
            ClearPackLinks();
        else
            LogLine(L"voice clarity removal failed");
        ControlServiceWait(L"Audiosrv", true);
        ControlServiceWait(L"AudioEndpointBuilder", true);
        if (voice < 0) {
            WriteText(kResult, L"Windows Ses Netliği kaldırılamadı. Mikrofon açılıyorsa efekt duyulmayabilir.");
            return;
        }
    }
    Sleep(400);
    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    comReady = SUCCEEDED(hr);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        LogHr(L"CoInitialize", hr);
        WriteText(kResult, L"COM başlatılamadı.");
        ControlServiceWait(L"Audiosrv", true);
        ControlServiceWait(L"AudioEndpointBuilder", true);
        return;
    }
    if (!WaitForCaptureDevice(deviceId)) {
        LogLine(L"device not ready");
        WriteText(kResult, L"Mikrofon henüz görünmedi. Ses açık. Birkaç saniye sonra Uygula'yı tekrar dene.");
        ControlServiceWait(L"Audiosrv", true);
        ControlServiceWait(L"AudioEndpointBuilder", true);
        if (comReady)
            CoUninitialize();
        return;
    }
    hr = AttachEndpoint(deviceId);
    LogHr(L"AttachEndpoint", hr);
    CoUninitialize();
    if (FAILED(hr)) {
        WriteText(kResult, L"Mikrofon özelliklerine yazılamadı. Kaldır ile geri alınabilir.");
        ControlServiceWait(L"Audiosrv", true);
        ControlServiceWait(L"AudioEndpointBuilder", true);
        return;
    }
    RecycleAudioEngine();
    {
        FILE *file = NULL;
        if (_wfopen_s(&file, kDeviceFile, L"wb") == 0 && file) {
            fwrite(deviceId, sizeof(wchar_t), wcslen(deviceId) + 1, file);
            fclose(file);
        }
    }
    if (voice == 1)
        WriteText(kResult, L"Hazır. Windows Ses Netliği efektin önüne geçiyordu, kaldırıldı. Ses yeniden başladı. Diğer programlarda bu mikrofonu seç.");
    else
        WriteText(kResult, L"Hazır. Efektler bu mikrofonda. Ses yeniden başladı. Diğer programlarda aynı mikrofonu seç.");
    if (!ControlServiceWait(L"Audiosrv", true) || !ControlServiceWait(L"AudioEndpointBuilder", true))
        LogLine(L"audio service restart failed");
    LogLine(L"install ok");
}

static void RemoveMain()
{
    LogLine(L"remove start");
    StripMikrafSlots();
    UnregisterCom();
    RestartAudio();
    if (!ControlServiceWait(L"Audiosrv", true) || !ControlServiceWait(L"AudioEndpointBuilder", true))
        LogLine(L"audio service restart failed");
    DeleteFileW(kDeviceFile);
    WriteText(kResult, L"Mikraf kalktı. Mikrofon kendi sesini verir. Ses yeniden başladı.");
    LogLine(L"remove ok");
}

extern "C" __declspec(dllexport) void CALLBACK InstallApo(HWND hwnd, HINSTANCE module, LPSTR command, int show)
{
    wchar_t deviceId[260];
    UNREFERENCED_PARAMETER(hwnd);
    UNREFERENCED_PARAMETER(module);
    UNREFERENCED_PARAMETER(show);
    deviceId[0] = 0;
    if (command)
        MultiByteToWideChar(CP_ACP, 0, command, -1, deviceId, 260);
    while (deviceId[0] == L' ')
        memmove(deviceId, deviceId + 1, (wcslen(deviceId) + 1) * sizeof(wchar_t));
    {
        size_t length = wcslen(deviceId);
        while (length > 0 && (deviceId[length - 1] == L' ' || deviceId[length - 1] == L'\r' || deviceId[length - 1] == L'\n'))
            deviceId[--length] = 0;
    }
    InstallMain(deviceId);
}

static const wchar_t kVoiceDevice[] = L"SWD\\DRIVERENUM\\{96bedf2c-18cb-4a15-b821-5e95ed0fea61}#VocaEffectPack&1&2ef7596b&0";

static int ReadInstalledDevice(wchar_t *deviceId, int count);
static void ReattachInstalled();

static CONFIGRET SetVoiceDevice(bool enable)
{
    DEVINST node = 0;
    CONFIGRET cr = CM_Locate_DevNodeW(&node, (DEVINSTID_W)kVoiceDevice, CM_LOCATE_DEVNODE_NORMAL);
    if (cr != CR_SUCCESS)
        return cr;
    if (enable)
        return CM_Enable_DevNode(node, 0);
    return CM_Disable_DevNode(node, CM_DISABLE_UI_NOT_OK);
}

extern "C" __declspec(dllexport) void CALLBACK VoiceOff(HWND hwnd, HINSTANCE module, LPSTR command, int show)
{
    CONFIGRET cr;
    wchar_t line[160];
    UNREFERENCED_PARAMETER(hwnd);
    UNREFERENCED_PARAMETER(module);
    UNREFERENCED_PARAMETER(command);
    UNREFERENCED_PARAMETER(show);
    CopyRuntime();
    cr = SetVoiceDevice(false);
    StringCchPrintfW(line, 160, L"voice device disable %lu", (unsigned long)cr);
    LogLine(line);
    if (cr != CR_SUCCESS) {
        WriteText(kResult, L"Ses Netliği aygıtı kapatılamadı.");
        return;
    }
    RestartAudio();
    Sleep(800);
    ReattachInstalled();
    RecycleAudioEngine();
    WriteText(kResult, L"Ses Netliği kapatıldı. Mikraf mikrofona yeniden bağlandı.");
    LogLine(L"voice device off");
}

extern "C" __declspec(dllexport) void CALLBACK VoiceOn(HWND hwnd, HINSTANCE module, LPSTR command, int show)
{
    CONFIGRET cr;
    wchar_t line[160];
    UNREFERENCED_PARAMETER(hwnd);
    UNREFERENCED_PARAMETER(module);
    UNREFERENCED_PARAMETER(command);
    UNREFERENCED_PARAMETER(show);
    cr = SetVoiceDevice(true);
    StringCchPrintfW(line, 160, L"voice device enable %lu", (unsigned long)cr);
    LogLine(line);
    RestartAudio();
    Sleep(800);
    ReattachInstalled();
    RecycleAudioEngine();
    WriteText(kResult, L"Ses Netliği yeniden açıldı.");
    LogLine(L"voice device on");
}

static const wchar_t kPackFx[] = L"SYSTEM\\CurrentControlSet\\Control\\Class\\{5989fce8-9cd0-467d-8a6a-5419e31529d4}\\0000\\EffectPackRegistration\\{348C62B2-0B77-466D-A7DB-E2AB84498212}\\FxProperties";
static const wchar_t kPackClsidBackup[] = L"C:\\ProgramData\\Mikraf\\pack-clsid-backup.bin";

static int WritePackClsid(const wchar_t *clsid)
{
    HKEY key = NULL;
    wchar_t multi[160];
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kPackFx, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
        return 0;
    RegSetValueExW(key, L"{D04E05A6-594B-4FB6-A80D-01AF5EED7D1D},6", 0, REG_SZ, (BYTE *)clsid, (DWORD)((wcslen(clsid) + 1) * sizeof(wchar_t)));
    ZeroMemory(multi, sizeof(multi));
    wcscpy_s(multi, 80, clsid);
    multi[wcslen(clsid) + 1] = 0;
    RegSetValueExW(key, L"{D04E05A6-594B-4fb6-A80D-01AF5EED7D1D},14", 0, REG_MULTI_SZ, (BYTE *)multi, (DWORD)((wcslen(clsid) + 2) * sizeof(wchar_t)));
    RegCloseKey(key);
    return 1;
}

static void DumpKey(HKEY key, FILE *file, const wchar_t *path)
{
    DWORD index = 0;
    wchar_t name[256];
    BYTE data[1024];
    DWORD nameLen, type, size;
    wchar_t line[700];
    while (1) {
        nameLen = 256;
        size = sizeof(data);
        if (RegEnumValueW(key, index, name, &nameLen, NULL, &type, data, &size) != ERROR_SUCCESS)
            break;
        index++;
        if (type == REG_SZ || type == REG_EXPAND_SZ)
            StringCchPrintfW(line, 700, L"%s :: %s = %s\n", path, name, (wchar_t *)data);
        else
            StringCchPrintfW(line, 700, L"%s :: %s type %lu size %lu\n", path, name, type, size);
        fputws(line, file);
    }
    index = 0;
    while (1) {
        HKEY child = NULL;
        nameLen = 256;
        if (RegEnumKeyExW(key, index, name, &nameLen, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        index++;
        if (RegOpenKeyExW(key, name, 0, KEY_READ | KEY_WOW64_64KEY, &child) == ERROR_SUCCESS) {
            wchar_t childPath[500];
            StringCchPrintfW(childPath, 500, L"%s\\%s", path, name);
            DumpKey(child, file, childPath);
            RegCloseKey(child);
        }
    }
}

extern "C" __declspec(dllexport) void CALLBACK DumpVoice(HWND hwnd, HINSTANCE module, LPSTR command, int show)
{
    HKEY key = NULL;
    FILE *file = NULL;
    const wchar_t *path = L"SYSTEM\\CurrentControlSet\\Enum\\SWD\\DRIVERENUM\\{96bedf2c-18cb-4a15-b821-5e95ed0fea61}#VocaEffectPack&1&2ef7596b&0";
    UNREFERENCED_PARAMETER(hwnd);
    UNREFERENCED_PARAMETER(module);
    UNREFERENCED_PARAMETER(command);
    UNREFERENCED_PARAMETER(show);
    if (_wfopen_s(&file, L"C:\\ProgramData\\Mikraf\\voice-props.txt", L"wb") != 0 || !file)
        return;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_READ | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS) {
        DumpKey(key, file, L"device");
        RegCloseKey(key);
    } else {
        fputws(L"device open failed\n", file);
    }
    fclose(file);
    WriteText(kResult, L"Ses Netliği kayıtları döküldü.");
}

extern "C" __declspec(dllexport) void CALLBACK SwapPack(HWND hwnd, HINSTANCE module, LPSTR command, int show)
{
    HKEY key = NULL;
    BYTE data[512];
    DWORD size;
    DWORD type = 0;
    FILE *file = NULL;
    UNREFERENCED_PARAMETER(hwnd);
    UNREFERENCED_PARAMETER(module);
    UNREFERENCED_PARAMETER(command);
    UNREFERENCED_PARAMETER(show);
    CopyRuntime();
    if (GetFileAttributesW(kPackClsidBackup) == INVALID_FILE_ATTRIBUTES && RegOpenKeyExW(HKEY_LOCAL_MACHINE, kPackFx, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS) {
        if (_wfopen_s(&file, kPackClsidBackup, L"wb") == 0 && file) {
            fwrite("PKCLBK01", 1, 8, file);
            size = sizeof(data);
            if (RegQueryValueExW(key, L"{D04E05A6-594B-4FB6-A80D-01AF5EED7D1D},6", NULL, &type, data, &size) == ERROR_SUCCESS) {
                fwrite(&size, 4, 1, file);
                fwrite(data, 1, size, file);
            }
            fclose(file);
        }
        RegCloseKey(key);
    }
    if (!WritePackClsid(kClsid)) {
        WriteText(kResult, L"Efekt paketinin sınıfı yazılamadı.");
        return;
    }
    RecycleAudioEngine();
    WriteText(kResult, L"Ses Netliği paketi Mikraf sınıfını kullanacak.");
    LogLine(L"pack swapped");
}

extern "C" __declspec(dllexport) void CALLBACK UnswapPack(HWND hwnd, HINSTANCE module, LPSTR command, int show)
{
    UNREFERENCED_PARAMETER(hwnd);
    UNREFERENCED_PARAMETER(module);
    UNREFERENCED_PARAMETER(command);
    UNREFERENCED_PARAMETER(show);
    if (!WritePackClsid(L"{9E6136E0-57AB-4949-B57A-3627BE142855}")) {
        WriteText(kResult, L"Ses Netliği sınıfı geri yazılamadı.");
        return;
    }
    RecycleAudioEngine();
    WriteText(kResult, L"Ses Netliği sınıfı geri alındı.");
    LogLine(L"pack unswapped");
}

extern "C" __declspec(dllexport) void CALLBACK DropPack(HWND hwnd, HINSTANCE module, LPSTR command, int show)
{
    wchar_t deviceId[260];
    wchar_t guid[80];
    UNREFERENCED_PARAMETER(hwnd);
    UNREFERENCED_PARAMETER(module);
    UNREFERENCED_PARAMETER(command);
    UNREFERENCED_PARAMETER(show);
    CopyRuntime();
    if (!ReadInstalledDevice(deviceId, 260) || !EndpointGuid(deviceId, guid, 80)) {
        WriteText(kResult, L"Mikrofon kaydı yok.");
        return;
    }
    DetachVoiceClarity(guid);
    RecycleAudioEngine();
    WriteText(kResult, L"Ses Netliği bağı bu mikrofondan alındı.");
    LogLine(L"pack dropped");
}

static const wchar_t kExcludePath[] = L"SYSTEM\\CurrentControlSet\\Control\\Class\\{5989fce8-9cd0-467d-8a6a-5419e31529d4}\\0000\\EffectPackRegistration\\{348C62B2-0B77-466D-A7DB-E2AB84498212}\\FxProperties";
static const wchar_t kExcludeValue[] = L"{6473F77A-DF09-41C9-B608-2D2420EA634F},2";
static const wchar_t kExcludeBackup[] = L"C:\\ProgramData\\Mikraf\\exclude-backup.bin";

static int ReadInstalledDevice(wchar_t *deviceId, int count)
{
    FILE *file = NULL;
    deviceId[0] = 0;
    if (_wfopen_s(&file, kDeviceFile, L"rb") != 0 || !file)
        return 0;
    if (fread(deviceId, sizeof(wchar_t), (size_t)count, file) > 0)
        deviceId[count - 1] = 0;
    fclose(file);
    return deviceId[0] != 0;
}

static void ReattachInstalled()
{
    wchar_t deviceId[260];
    HRESULT hr;
    if (!ReadInstalledDevice(deviceId, 260))
        return;
    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
        return;
    hr = AttachEndpoint(deviceId);
    LogHr(L"Reattach", hr);
    CoUninitialize();
}

static const wchar_t kSlotBackup[] = L"C:\\ProgramData\\Mikraf\\slot-backup.bin";
static const wchar_t kVcSlotName[] = L"{9e6136e0-57ab-4949-b57a-3627be142855},100";
static const wchar_t kOurSlotName[] = L"{7c3e9a21-5b14-4e6a-9f02-6d8a4c1e2b70},100";
static const wchar_t kEndpointMfx[] = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},6";
static const wchar_t kEndpointComposite[] = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},14";

static int OpenFifineFx(HKEY *key)
{
    wchar_t deviceId[260];
    wchar_t guid[80];
    wchar_t path[420];
    if (!ReadInstalledDevice(deviceId, 260) || !EndpointGuid(deviceId, guid, 80))
        return 0;
    StringCchPrintfW(path, 420, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Capture\\%s\\FxProperties", guid);
    return RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY, key) == ERROR_SUCCESS;
}

static void WriteEndpointMfx(HKEY key)
{
    wchar_t multi[160];
    RegSetValueExW(key, kEndpointMfx, 0, REG_SZ, (BYTE *)kClsid, (DWORD)((wcslen(kClsid) + 1) * sizeof(wchar_t)));
    ZeroMemory(multi, sizeof(multi));
    wcscpy_s(multi, 80, kClsid);
    multi[wcslen(kClsid) + 1] = 0;
    RegSetValueExW(key, kEndpointComposite, 0, REG_MULTI_SZ, (BYTE *)multi, (DWORD)((wcslen(kClsid) + 2) * sizeof(wchar_t)));
}

extern "C" __declspec(dllexport) void CALLBACK SetMfx(HWND hwnd, HINSTANCE module, LPSTR command, int show)
{
    HKEY key = NULL;
    UNREFERENCED_PARAMETER(hwnd);
    UNREFERENCED_PARAMETER(module);
    UNREFERENCED_PARAMETER(command);
    UNREFERENCED_PARAMETER(show);
    CopyRuntime();
    DeleteFileW(L"C:\\ProgramData\\Mikraf\\apo-load.log");
    if (!OpenFifineFx(&key)) {
        WriteText(kResult, L"Mikrofon kaydı yok.");
        return;
    }
    WriteEndpointMfx(key);
    RegCloseKey(key);
    RecycleAudioEngine();
    ReattachInstalled();
    WriteText(kResult, L"Mikraf bu mikrofonun mod efektine yazıldı.");
    LogLine(L"endpoint mfx set");
}

extern "C" __declspec(dllexport) void CALLBACK TakeSlot(HWND hwnd, HINSTANCE module, LPSTR command, int show)
{
    HKEY key = NULL;
    BYTE data[1024];
    DWORD type = 0;
    DWORD size = sizeof(data);
    FILE *file = NULL;
    UNREFERENCED_PARAMETER(hwnd);
    UNREFERENCED_PARAMETER(module);
    UNREFERENCED_PARAMETER(command);
    UNREFERENCED_PARAMETER(show);
    CopyRuntime();
    DeleteFileW(L"C:\\ProgramData\\Mikraf\\apo-load.log");
    if (!OpenFifineFx(&key)) {
        WriteText(kResult, L"Mikrofon kaydı yok.");
        return;
    }
    if (RegQueryValueExW(key, kVcSlotName, NULL, &type, data, &size) != ERROR_SUCCESS || type != REG_SZ || size < 4 || size > sizeof(data)) {
        RegCloseKey(key);
        WriteText(kResult, L"Ses Netliği yuvası bu mikrofonda yok.");
        return;
    }
    if (GetFileAttributesW(kSlotBackup) == INVALID_FILE_ATTRIBUTES && _wfopen_s(&file, kSlotBackup, L"wb") == 0 && file) {
        fwrite("SLOTBK01", 1, 8, file);
        fwrite(&size, 4, 1, file);
        fwrite(data, 1, size, file);
        fclose(file);
    }
    if (RegSetValueExW(key, kOurSlotName, 0, REG_SZ, data, size) != ERROR_SUCCESS) {
        RegCloseKey(key);
        WriteText(kResult, L"Mikraf yuvası yazılamadı.");
        return;
    }
    RegDeleteValueW(key, kVcSlotName);
    WriteEndpointMfx(key);
    RegCloseKey(key);
    RecycleAudioEngine();
    ReattachInstalled();
    WriteText(kResult, L"Bu mikrofonun efekt yuvası Mikraf oldu.");
    LogLine(L"slot taken");
}

extern "C" __declspec(dllexport) void CALLBACK UndoSlot(HWND hwnd, HINSTANCE module, LPSTR command, int show)
{
    HKEY key = NULL;
    FILE *file = NULL;
    char magic[8];
    DWORD size = 0;
    BYTE *data = NULL;
    UNREFERENCED_PARAMETER(hwnd);
    UNREFERENCED_PARAMETER(module);
    UNREFERENCED_PARAMETER(command);
    UNREFERENCED_PARAMETER(show);
    if (!OpenFifineFx(&key)) {
        WriteText(kResult, L"Mikrofon kaydı yok.");
        return;
    }
    RegDeleteValueW(key, kOurSlotName);
    RegDeleteValueW(key, kEndpointMfx);
    RegDeleteValueW(key, kEndpointComposite);
    if (_wfopen_s(&file, kSlotBackup, L"rb") == 0 && file) {
        if (fread(magic, 1, 8, file) == 8 && memcmp(magic, "SLOTBK01", 8) == 0 && fread(&size, 4, 1, file) == 1 && size >= 4 && size <= 4096) {
            data = (BYTE *)malloc(size);
            if (data && fread(data, 1, size, file) == size)
                RegSetValueExW(key, kVcSlotName, 0, REG_SZ, data, size);
            free(data);
        }
        fclose(file);
    }
    RegCloseKey(key);
    RecycleAudioEngine();
    ReattachInstalled();
    WriteText(kResult, L"Mikrofonun efekt yuvası eski haline döndü.");
    LogLine(L"slot undone");
}

static const wchar_t kVcInproc[] = L"SYSTEM\\CurrentControlSet\\Control\\Class\\{5989fce8-9cd0-467d-8a6a-5419e31529d4}\\0000\\Classes\\CLSID\\{9E6136E0-57AB-4949-B57A-3627BE142855}\\InProcServer32";

extern "C" __declspec(dllexport) void CALLBACK RedirectVc(HWND hwnd, HINSTANCE module, LPSTR command, int show)
{
    HKEY key = NULL;
    BYTE data[1024];
    DWORD type = 0;
    DWORD size = sizeof(data);
    FILE *file = NULL;
    wchar_t dllPath[MAX_PATH];
    UNREFERENCED_PARAMETER(hwnd);
    UNREFERENCED_PARAMETER(module);
    UNREFERENCED_PARAMETER(command);
    UNREFERENCED_PARAMETER(show);
    RecycleAudioEngine();
    if (!CopyRuntime()) {
        WriteText(kResult, L"Mikraf dosyaları kopyalanamadı.");
        return;
    }
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kVcInproc, 0, KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
        WriteText(kResult, L"Ses Netliği sınıfı açılamadı.");
        return;
    }
    if (RegQueryValueExW(key, NULL, NULL, &type, data, &size) != ERROR_SUCCESS || size < sizeof(wchar_t) || size > sizeof(data)) {
        RegCloseKey(key);
        WriteText(kResult, L"Ses Netliği yolu okunamadı.");
        return;
    }
    if (!wcsstr((wchar_t *)data, L"mikraf-apo.dll")) {
        if (_wfopen_s(&file, kVcDllBackup, L"wb") == 0 && file) {
            fwrite("VCDLL001", 1, 8, file);
            fwrite(&size, 4, 1, file);
            fwrite(&type, 4, 1, file);
            fwrite(data, 1, size, file);
            fclose(file);
        }
        StringCchPrintfW(dllPath, MAX_PATH, L"%s\\mikraf-apo.dll", kInstallDir);
        if (RegSetValueExW(key, NULL, 0, REG_SZ, (BYTE *)dllPath, (DWORD)((wcslen(dllPath) + 1) * sizeof(wchar_t))) != ERROR_SUCCESS) {
            RegCloseKey(key);
            WriteText(kResult, L"Ses Netliği yolu yazılamadı.");
            return;
        }
    }
    RegCloseKey(key);
    {
        wchar_t keyPath[320];
        HKEY com = NULL;
        wchar_t dllPath[MAX_PATH];
        DWORD one = 1;
        DWORD flags = 0x0c;
        DWORD maxInstances = 0xffffffffu;
        const wchar_t *iface = L"{FD7F2B29-24D0-4B5C-B177-592C39F9CA10}";
        StringCchPrintfW(dllPath, MAX_PATH, L"%s\\mikraf-apo.dll", kInstallDir);
        StringCchPrintfW(keyPath, 320, L"SOFTWARE\\Classes\\CLSID\\{9E6136E0-57AB-4949-B57A-3627BE142855}");
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, keyPath, 0, NULL, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, NULL, &com, NULL) == ERROR_SUCCESS) {
            RegSetValueExW(com, NULL, 0, REG_SZ, (BYTE *)L"Mikraf", sizeof(L"Mikraf"));
            RegCloseKey(com);
        }
        StringCchPrintfW(keyPath, 320, L"SOFTWARE\\Classes\\CLSID\\{9E6136E0-57AB-4949-B57A-3627BE142855}\\InprocServer32");
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, keyPath, 0, NULL, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, NULL, &com, NULL) == ERROR_SUCCESS) {
            RegSetValueExW(com, NULL, 0, REG_SZ, (BYTE *)dllPath, (DWORD)((wcslen(dllPath) + 1) * sizeof(wchar_t)));
            RegSetValueExW(com, L"ThreadingModel", 0, REG_SZ, (BYTE *)L"Both", sizeof(L"Both"));
            RegCloseKey(com);
        }
        StringCchPrintfW(keyPath, 320, L"SOFTWARE\\Classes\\AudioEngine\\AudioProcessingObjects\\{9E6136E0-57AB-4949-B57A-3627BE142855}");
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, keyPath, 0, NULL, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, NULL, &com, NULL) == ERROR_SUCCESS) {
            RegSetValueExW(com, L"FriendlyName", 0, REG_SZ, (BYTE *)L"Mikraf", sizeof(L"Mikraf"));
            RegSetValueExW(com, L"Copyright", 0, REG_SZ, (BYTE *)L"Mikraf", sizeof(L"Mikraf"));
            RegSetValueExW(com, L"MajorVersion", 0, REG_DWORD, (BYTE *)&one, sizeof(one));
            RegSetValueExW(com, L"MinorVersion", 0, REG_DWORD, (BYTE *)&one, sizeof(one));
            RegSetValueExW(com, L"Flags", 0, REG_DWORD, (BYTE *)&flags, sizeof(flags));
            RegSetValueExW(com, L"MinInputConnections", 0, REG_DWORD, (BYTE *)&one, sizeof(one));
            RegSetValueExW(com, L"MaxInputConnections", 0, REG_DWORD, (BYTE *)&one, sizeof(one));
            RegSetValueExW(com, L"MinOutputConnections", 0, REG_DWORD, (BYTE *)&one, sizeof(one));
            RegSetValueExW(com, L"MaxOutputConnections", 0, REG_DWORD, (BYTE *)&one, sizeof(one));
            RegSetValueExW(com, L"MaxInstances", 0, REG_DWORD, (BYTE *)&maxInstances, sizeof(maxInstances));
            RegSetValueExW(com, L"NumAPOInterfaces", 0, REG_DWORD, (BYTE *)&one, sizeof(one));
            RegSetValueExW(com, L"APOInterface0", 0, REG_SZ, (BYTE *)iface, (DWORD)((wcslen(iface) + 1) * sizeof(wchar_t)));
            RegCloseKey(com);
        }
        if (OpenFifineFx(&key)) {
            WriteEndpointMfx(key);
            RegCloseKey(key);
        }
    }
    WritePackClsid(L"{9E6136E0-57AB-4949-B57A-3627BE142855}");
    RestartAudio();
    Sleep(800);
    ReattachInstalled();
    if (OpenFifineFx(&key)) {
        WriteEndpointMfx(key);
        RegCloseKey(key);
    }
    DeleteFileW(L"C:\\ProgramData\\Mikraf\\apo-load.log");
    RecycleAudioEngine();
    WriteText(kResult, L"Fifine, Mikraf zincirini doğrudan kullanacak. Diğer mikrofonlar Ses Netliği'nde kalır.");
    LogLine(L"voice clarity redirected");
}

extern "C" __declspec(dllexport) void CALLBACK UndoRedirect(HWND hwnd, HINSTANCE module, LPSTR command, int show)
{
    HKEY key = NULL;
    FILE *file = NULL;
    char magic[8];
    DWORD size = 0;
    DWORD type = REG_EXPAND_SZ;
    BYTE *data = NULL;
    UNREFERENCED_PARAMETER(hwnd);
    UNREFERENCED_PARAMETER(module);
    UNREFERENCED_PARAMETER(command);
    UNREFERENCED_PARAMETER(show);
    if (_wfopen_s(&file, kVcDllBackup, L"rb") != 0 || !file) {
        WriteText(kResult, L"Ses Netliği yolu yedeği yok.");
        return;
    }
    if (fread(magic, 1, 8, file) != 8 || memcmp(magic, "VCDLL001", 8) != 0 || fread(&size, 4, 1, file) != 1 || fread(&type, 4, 1, file) != 1 || size < 4 || size > 2048) {
        fclose(file);
        WriteText(kResult, L"Ses Netliği yolu yedeği bozuk.");
        return;
    }
    data = (BYTE *)malloc(size);
    if (!data || fread(data, 1, size, file) != size) {
        free(data);
        fclose(file);
        WriteText(kResult, L"Ses Netliği yolu yedeği okunamadı.");
        return;
    }
    fclose(file);
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kVcInproc, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS) {
        RegSetValueExW(key, NULL, 0, type, data, size);
        RegCloseKey(key);
    }
    free(data);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\CLSID\\{9E6136E0-57AB-4949-B57A-3627BE142855}");
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\AudioEngine\\AudioProcessingObjects\\{9E6136E0-57AB-4949-B57A-3627BE142855}");
    if (OpenFifineFx(&key)) {
        RegDeleteValueW(key, kEndpointMfx);
        RegDeleteValueW(key, kEndpointComposite);
        RegCloseKey(key);
    }
    RecycleAudioEngine();
    WriteText(kResult, L"Ses Netliği kendi dosyasına döndü.");
    LogLine(L"voice clarity redirect undone");
}

extern "C" __declspec(dllexport) void CALLBACK ExcludeFifine(HWND hwnd, HINSTANCE module, LPSTR command, int show)
{
    HKEY key = NULL;
    DWORD type = 0;
    DWORD size = 0;
    BYTE *data = NULL;
    wchar_t *text;
    wchar_t *end;
    const wchar_t *pattern = L"*VID_3142&PID_A601*";
    int found = 0;
    FILE *file = NULL;
    LONG rc;
    UNREFERENCED_PARAMETER(hwnd);
    UNREFERENCED_PARAMETER(module);
    UNREFERENCED_PARAMETER(command);
    UNREFERENCED_PARAMETER(show);
    CopyRuntime();
    rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kExcludePath, 0, KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY, &key);
    if (rc != ERROR_SUCCESS) {
        WriteText(kResult, L"Ses Netliği listesi açılamadı.");
        LogHr(L"exclude open", HRESULT_FROM_WIN32(rc));
        return;
    }
    RegQueryValueExW(key, kExcludeValue, NULL, &type, NULL, &size);
    if (type != REG_MULTI_SZ || size < sizeof(wchar_t) * 2 || size > 65536) {
        RegCloseKey(key);
        WriteText(kResult, L"Ses Netliği listesi okunamadı.");
        return;
    }
    data = (BYTE *)malloc((size_t)size + 256);
    size = size;
    if (!data || RegQueryValueExW(key, kExcludeValue, NULL, &type, data, &size) != ERROR_SUCCESS) {
        free(data);
        RegCloseKey(key);
        WriteText(kResult, L"Ses Netliği listesi okunamadı.");
        return;
    }
    text = (wchar_t *)data;
    for (end = text; *end; end += wcslen(end) + 1) {
        if (wcsstr(end, L"VID_3142&PID_A601"))
            found = 1;
    }
    if (!found) {
        size_t room;
        if (GetFileAttributesW(kExcludeBackup) == INVALID_FILE_ATTRIBUTES && _wfopen_s(&file, kExcludeBackup, L"wb") == 0 && file) {
            fwrite("EXCLBK01", 1, 8, file);
            fwrite(&size, 4, 1, file);
            fwrite(data, 1, size, file);
            fclose(file);
        }
        room = ((size_t)size + 256) / sizeof(wchar_t) - (size_t)(end - text);
        if (wcscpy_s(end, room, pattern) != 0) {
            free(data);
            RegCloseKey(key);
            WriteText(kResult, L"Fifine dışlama satırı sığmadı.");
            return;
        }
        end += wcslen(pattern) + 1;
        *end = 0;
        rc = RegSetValueExW(key, kExcludeValue, 0, REG_MULTI_SZ, data, (DWORD)((end - text + 1) * sizeof(wchar_t)));
        if (rc != ERROR_SUCCESS) {
            free(data);
            RegCloseKey(key);
            WriteText(kResult, L"Ses Netliği listesi yazılamadı.");
            LogHr(L"exclude write", HRESULT_FROM_WIN32(rc));
            return;
        }
        LogLine(L"fifine excluded");
    } else {
        LogLine(L"fifine already excluded");
    }
    free(data);
    RegCloseKey(key);
    RestartAudio();
    Sleep(600);
    ReattachInstalled();
    RecycleAudioEngine();
    WriteText(kResult, L"Fifine, Ses Netliği listesinden çıkarıldı. Mikrofon yeniden bağlandı.");
    LogLine(L"exclude done");
}

extern "C" __declspec(dllexport) void CALLBACK UndoExclude(HWND hwnd, HINSTANCE module, LPSTR command, int show)
{
    FILE *file = NULL;
    char magic[8];
    DWORD size = 0;
    BYTE *data = NULL;
    HKEY key = NULL;
    UNREFERENCED_PARAMETER(hwnd);
    UNREFERENCED_PARAMETER(module);
    UNREFERENCED_PARAMETER(command);
    UNREFERENCED_PARAMETER(show);
    if (_wfopen_s(&file, kExcludeBackup, L"rb") != 0 || !file) {
        WriteText(kResult, L"Dışlama yedeği yok.");
        return;
    }
    if (fread(magic, 1, 8, file) != 8 || memcmp(magic, "EXCLBK01", 8) != 0 || fread(&size, 4, 1, file) != 1 || size < 4 || size > 65536) {
        fclose(file);
        WriteText(kResult, L"Dışlama yedeği bozuk.");
        return;
    }
    data = (BYTE *)malloc(size);
    if (!data || fread(data, 1, size, file) != size) {
        free(data);
        fclose(file);
        WriteText(kResult, L"Dışlama yedeği okunamadı.");
        return;
    }
    fclose(file);
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kExcludePath, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS) {
        RegSetValueExW(key, kExcludeValue, 0, REG_MULTI_SZ, data, size);
        RegCloseKey(key);
    }
    free(data);
    {
        wchar_t deviceId[260];
        wchar_t guid[80];
        wchar_t path[420];
        if (ReadInstalledDevice(deviceId, 260) && EndpointGuid(deviceId, guid, 80)) {
            StringCchPrintfW(path, 420, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Capture\\%s\\FxProperties\\{ec58d46d-db28-4fd3-bfc2-051c5a3738fc}\\User", guid);
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS) {
                RegDeleteValueW(key, L"{c050f28d-e127-4336-8dc5-e65320c09dfe},1");
                RegCloseKey(key);
                key = NULL;
            }
        }
    }
    RestartAudio();
    Sleep(600);
    ReattachInstalled();
    RecycleAudioEngine();
    WriteText(kResult, L"Ses Netliği dışlama listesi eski haline döndü.");
    LogLine(L"exclude undone");
}

extern "C" __declspec(dllexport) void CALLBACK SetVoiceOff(HWND hwnd, HINSTANCE module, LPSTR command, int show)
{
    wchar_t deviceId[260];
    wchar_t guid[80];
    wchar_t path[420];
    FILE *file = NULL;
    HKEY key = NULL;
    DWORD off = 0;
    UNREFERENCED_PARAMETER(hwnd);
    UNREFERENCED_PARAMETER(module);
    UNREFERENCED_PARAMETER(command);
    UNREFERENCED_PARAMETER(show);
    deviceId[0] = 0;
    if (_wfopen_s(&file, kDeviceFile, L"rb") == 0 && file) {
        if (fread(deviceId, sizeof(wchar_t), 260, file) > 0)
            deviceId[259] = 0;
        fclose(file);
    }
    if (!EndpointGuid(deviceId, guid, 80)) {
        WriteText(kResult, L"Ses Netliği kapatılamadı.");
        return;
    }
    CopyRuntime();
    StringCchPrintfW(path, 420, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Capture\\%s\\FxProperties\\{ec58d46d-db28-4fd3-bfc2-051c5a3738fc}\\User", guid);
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, path, 0, NULL, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, NULL, &key, NULL) != ERROR_SUCCESS) {
        WriteText(kResult, L"Ses Netliği anahtarı yazılamadı.");
        return;
    }
    RegSetValueExW(key, L"{c050f28d-e127-4336-8dc5-e65320c09dfe},1", 0, REG_DWORD, (BYTE *)&off, sizeof(off));
    RegCloseKey(key);
    RecycleAudioEngine();
    WriteText(kResult, L"Ses Netliği etkisi bu mikrofonda kapatıldı. Paket duruyor.");
    LogLine(L"voice effect off");
}

extern "C" __declspec(dllexport) void CALLBACK RestoreVoice(HWND hwnd, HINSTANCE module, LPSTR command, int show)
{
    wchar_t deviceId[260];
    wchar_t guid[80];
    FILE *file = NULL;
    UNREFERENCED_PARAMETER(hwnd);
    UNREFERENCED_PARAMETER(module);
    UNREFERENCED_PARAMETER(command);
    UNREFERENCED_PARAMETER(show);
    deviceId[0] = 0;
    if (_wfopen_s(&file, kDeviceFile, L"rb") == 0 && file) {
        if (fread(deviceId, sizeof(wchar_t), 260, file) > 0)
            deviceId[259] = 0;
        fclose(file);
    }
    if (!EndpointGuid(deviceId, guid, 80)) {
        WriteText(kResult, L"Ses Netliği geri alınamadı: mikrofon kaydı yok.");
        return;
    }
    RestoreVoiceClarity(guid);
    RecycleAudioEngine();
    WriteText(kResult, L"Ses Netliği bu mikrofona geri alındı.");
    LogLine(L"voice clarity restored");
}

static const wchar_t kEqSfxBackup[] = L"C:\\ProgramData\\Mikraf\\eqapo-sfx.txt";
static const wchar_t kSfxValue[] = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5";

static int ReadSz(HKEY key, const wchar_t *name, wchar_t *out, int count)
{
    DWORD type = 0;
    DWORD size = (DWORD)count * sizeof(wchar_t);
    out[0] = 0;
    if (RegQueryValueExW(key, name, NULL, &type, (BYTE *)out, &size) != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ))
        return 0;
    out[count - 1] = 0;
    return 1;
}

extern "C" __declspec(dllexport) void CALLBACK TakeSfx(HWND hwnd, HINSTANCE module, LPSTR command, int show)
{
    HKEY key = NULL;
    wchar_t current[80];
    FILE *file = NULL;
    UNREFERENCED_PARAMETER(hwnd);
    UNREFERENCED_PARAMETER(module);
    UNREFERENCED_PARAMETER(command);
    UNREFERENCED_PARAMETER(show);
    DeleteFileW(L"C:\\ProgramData\\Mikraf\\apo-load.log");
    RecycleAudioEngine();
    if (!CopyRuntime()) {
        WriteText(kResult, L"Mikraf DLL kopyalanamadı.");
        return;
    }
    if (!OpenFifineFx(&key)) {
        WriteText(kResult, L"Fifine kaydı yok.");
        return;
    }
    current[0] = 0;
    if (ReadSz(key, kSfxValue, current, 80) && _wcsicmp(current, kClsid) != 0) {
        if (_wfopen_s(&file, kEqSfxBackup, L"wb") == 0 && file) {
            fwrite(current, sizeof(wchar_t), wcslen(current) + 1, file);
            fclose(file);
        }
    }
    if (RegSetValueExW(key, kSfxValue, 0, REG_SZ, (BYTE *)kClsid, (DWORD)((wcslen(kClsid) + 1) * sizeof(wchar_t))) != ERROR_SUCCESS) {
        RegCloseKey(key);
        WriteText(kResult, L"Fifine efekt yuvasına yazılamadı.");
        return;
    }
    RegCloseKey(key);
    RecycleAudioEngine();
    WriteText(kResult, L"Fifine efekt yuvası Mikraf.");
    LogLine(L"sfx taken");
}

extern "C" __declspec(dllexport) void CALLBACK RestoreSfx(HWND hwnd, HINSTANCE module, LPSTR command, int show)
{
    HKEY key = NULL;
    wchar_t current[80];
    FILE *file = NULL;
    UNREFERENCED_PARAMETER(hwnd);
    UNREFERENCED_PARAMETER(module);
    UNREFERENCED_PARAMETER(command);
    UNREFERENCED_PARAMETER(show);
    current[0] = 0;
    if (_wfopen_s(&file, kEqSfxBackup, L"rb") != 0 || !file) {
        WriteText(kResult, L"Equalizer APO yuvası yedeği yok.");
        return;
    }
    if (fread(current, sizeof(wchar_t), 79, file) < 2) {
        fclose(file);
        WriteText(kResult, L"Equalizer APO yuvası yedeği bozuk.");
        return;
    }
    fclose(file);
    current[79] = 0;
    if (!OpenFifineFx(&key)) {
        WriteText(kResult, L"Fifine kaydı yok.");
        return;
    }
    RegSetValueExW(key, kSfxValue, 0, REG_SZ, (BYTE *)current, (DWORD)((wcslen(current) + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    RecycleAudioEngine();
    WriteText(kResult, L"Fifine efekt yuvası Equalizer APO'ya döndü.");
    LogLine(L"sfx restored");
}

extern "C" __declspec(dllexport) void CALLBACK ClearPacks(HWND hwnd, HINSTANCE module, LPSTR command, int show)
{
    HKEY capture = NULL;
    DWORD index = 0;
    int links = 0;
    int slots = 0;
    wchar_t line[160];
    const wchar_t *root = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Capture";
    UNREFERENCED_PARAMETER(hwnd);
    UNREFERENCED_PARAMETER(module);
    UNREFERENCED_PARAMETER(command);
    UNREFERENCED_PARAMETER(show);
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, root, 0, KEY_READ | KEY_WOW64_64KEY, &capture) != ERROR_SUCCESS) {
        WriteText(kResult, L"Mikrofon listesi açılamadı.");
        return;
    }
    for (;;) {
        wchar_t name[80];
        DWORD nameLen = 80;
        wchar_t path[460];
        wchar_t value[520];
        HKEY fx = NULL;
        HKEY prop = NULL;
        int hadLink = 0;
        if (RegEnumKeyExW(capture, index++, name, &nameLen, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        StringCchPrintfW(path, 460, L"%s\\%s\\FxProperties", root, name);
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY, &fx) != ERROR_SUCCESS)
            continue;
        if (ReadSz(fx, L"{9e6136e0-57ab-4949-b57a-3627be142855},100", value, 520)) {
            RegDeleteValueW(fx, L"{9e6136e0-57ab-4949-b57a-3627be142855},100");
            hadLink = 1;
            links++;
        }
        if (ReadSz(fx, L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5", value, 520) && _wcsicmp(value, kClsid) == 0) {
            RegDeleteValueW(fx, L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5");
            slots++;
        }
        if (ReadSz(fx, L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},13", value, 520) && _wcsicmp(value, kClsid) == 0)
            RegDeleteValueW(fx, L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},13");
        if (ReadSz(fx, L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},4", value, 520) && _wcsicmp(value, L"Mikraf") == 0)
            RegDeleteValueW(fx, L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},4");
        if (_wcsicmp(name, L"{46ddf4f5-bdd7-42cb-9f86-d55ae2a56275}") == 0) {
            DWORD enabled = 0;
            RegSetValueExW(fx, L"{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},5", 0, REG_DWORD, (BYTE *)&enabled, sizeof(enabled));
        }
        RegCloseKey(fx);
        if (!hadLink)
            continue;
        StringCchPrintfW(path, 460, L"%s\\%s\\Properties", root, name);
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &prop) == ERROR_SUCCESS) {
            RegDeleteValueW(prop, L"{c876062a-a276-4ed6-b15b-3962e69007f8},2");
            RegDeleteValueW(prop, L"{c876062a-a276-4ed6-b15b-3962e69007f8},3");
            RegCloseKey(prop);
        }
    }
    RegCloseKey(capture);
    RecycleAudioEngine();
    StringCchPrintfW(line, 160, L"Ses Netliği sürücüsü kalktı. Kalan bağ %d, Mikraf yuvası %d temizlendi.", links, slots);
    WriteText(kResult, line);
    LogLine(line);
}

extern "C" __declspec(dllexport) void CALLBACK RemoveApo(HWND hwnd, HINSTANCE module, LPSTR command, int show)
{
    UNREFERENCED_PARAMETER(hwnd);
    UNREFERENCED_PARAMETER(module);
    UNREFERENCED_PARAMETER(command);
    UNREFERENCED_PARAMETER(show);
    RemoveMain();
}

extern "C" __declspec(dllexport) int MikrafApoPayloadBytes(void)
{
    return (int)sizeof(MikrafApoPayload);
}

extern "C" __declspec(dllexport) int MikrafApoPush(const MikrafApoPayload *payload)
{
    HANDLE handle;
    MikrafApoBlock *block = NULL;
    if (!payload)
        return 0;
    SavePayload(payload);
    handle = OpenBlock(1, &block);
    if (!handle || !block)
        return 2;
    InterlockedIncrement((volatile LONG *)&block->seq);
    MemoryBarrier();
    block->payload = *payload;
    MemoryBarrier();
    InterlockedIncrement((volatile LONG *)&block->seq);
    UnmapViewOfFile(block);
    CloseHandle(handle);
    return 1;
}

extern "C" __declspec(dllexport) int MikrafApoQuery(const wchar_t *deviceId)
{
    wchar_t guid[80];
    wchar_t path[360];
    HKEY key = NULL;
    BYTE data[1024];
    DWORD size = sizeof(data);
    DWORD type = 0;
    const wchar_t *needle = L"7C3E9A21-5B14-4E6A-9F02-6D8A4C1E2B70";
    if (!EndpointGuid(deviceId, guid, 80))
        return -1;
    StringCchPrintfW(path, 360, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Capture\\%s\\FxProperties", guid);
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
        return 0;
    size = sizeof(data);
    if (RegQueryValueExW(key, L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5", NULL, &type, data, &size) == ERROR_SUCCESS) {
        for (DWORD i = 0; i + 37 * sizeof(wchar_t) <= size; i += sizeof(wchar_t)) {
            if (_wcsnicmp((wchar_t *)(data + i), needle, 36) == 0) {
                RegCloseKey(key);
                return 1;
            }
        }
    }
    RegCloseKey(key);
    return 0;
}

extern "C" __declspec(dllexport) int MikrafApoLive(int *count, int *rate, int *channels)
{
    HANDLE handle = OpenFileMappingW(FILE_MAP_READ, FALSE, kMapName);
    MikrafApoBlock *block;
    if (!handle)
        return 0;
    block = (MikrafApoBlock *)MapViewOfFile(handle, FILE_MAP_READ, 0, 0, sizeof(MikrafApoBlock));
    if (!block || block->magic != MIKRAF_APO_MAGIC) {
        if (block)
            UnmapViewOfFile(block);
        CloseHandle(handle);
        return 0;
    }
    if (count)
        *count = (int)block->processCount;
    if (rate)
        *rate = (int)block->sampleRate;
    if (channels)
        *channels = (int)block->channelCount;
    UnmapViewOfFile(block);
    CloseHandle(handle);
    return 1;
}

extern "C" __declspec(dllexport) int MikrafApoSelfTest(void)
{
    Dsp dsp;
    MikrafApoPayload settings;
    float input[480];
    float output[480];
    float peak = 0.f;
    int block;
    int i;
    if (sizeof(MikrafApoPayload) != 216)
        return 1;
    ZeroMemory(&dsp, sizeof(dsp));
    LoadSpeex();
    if (!g_spxOpen)
        return 2;
    if (!dsp_prepare(&dsp, 48000, 1, 480, g_spxOpen, g_spxClose, g_spxRun))
        return 3;
    mikraf_settings_default(&settings);
    settings.nsOn = 0;
    settings.chainCount = 2;
    settings.chain[0] = MIKRAF_ST_COMP;
    settings.chain[1] = MIKRAF_ST_LIM;
    for (block = 0; block < 16; block++) {
        for (i = 0; i < 480; i++)
            input[i] = 0.9f * sinf(2.f * 3.14159265f * 440.f * (float)(block * 480 + i) / 48000.f);
        dsp_process(&dsp, &settings, input, output, 480);
        if (block >= 8) {
            for (i = 0; i < 480; i++) {
                float a;
                if (output[i] != output[i]) {
                    dsp_release(&dsp);
                    return 4;
                }
                a = output[i] < 0.f ? -output[i] : output[i];
                if (a > peak)
                    peak = a;
            }
        }
    }
    dsp_release(&dsp);
    if (peak > 1.02f || peak < 0.2f)
        return 5;
    ZeroMemory(&dsp, sizeof(dsp));
    if (!dsp_prepare(&dsp, 48000, 1, 480, g_spxOpen, g_spxClose, g_spxRun))
        return 6;
    mikraf_settings_default(&settings);
    settings.chainCount = 1;
    settings.chain[0] = MIKRAF_ST_GAIN;
    settings.gainOn = 1;
    settings.gainDb = 6.f;
    settings.nsOn = 0;
    input[0] = 0.1f;
    for (i = 1; i < 480; i++)
        input[i] = 0.1f;
    dsp_process(&dsp, &settings, input, output, 480);
    if (output[10] < 0.19f || output[10] > 0.21f) {
        dsp_release(&dsp);
        return 7;
    }
    settings.chainCount = 0;
    input[0] = 0.25f;
    for (i = 1; i < 480; i++)
        input[i] = 0.25f;
    dsp_process(&dsp, &settings, input, output, 480);
    dsp_release(&dsp);
    if (output[3] < 0.249f || output[3] > 0.251f)
        return 8;
    return 0;
}

STDAPI DllRegisterServer(void)
{
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(g_module, path, MAX_PATH))
        return E_FAIL;
    return RegisterCom(path);
}

STDAPI DllUnregisterServer(void)
{
    UnregisterCom();
    return S_OK;
}
