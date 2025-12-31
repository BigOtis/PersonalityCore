param(
  [string]$ProjectRoot = $(Resolve-Path "."),
  [string]$Text = "Hello from LocalTalker."
)

$ErrorActionPreference = "Stop"

function Fail($msg) {
  Write-Host ""
  Write-Host "FAIL: $msg" -ForegroundColor Red
  exit 1
}

function Ok($msg) {
  Write-Host "OK: $msg" -ForegroundColor Green
}

$plugin = Join-Path $ProjectRoot "Plugins\\LocalTalker"
if (!(Test-Path $plugin)) { Fail "Plugin not found: $plugin" }

$llamaDir  = Join-Path $plugin "ThirdParty\\llama\\Win64\\Release"
$piperDir  = Join-Path $plugin "ThirdParty\\piper\\Win64\\Release"
$modelsDir = Join-Path $plugin "Resources\\Models"
$voicesDir = Join-Path $plugin "Resources\\Voices"
$notices   = Join-Path $plugin "Resources\\ThirdPartyNotices"

$libllama  = Join-Path $llamaDir "libllama.dll"
$ggmlBase  = Join-Path $llamaDir "ggml-base.dll"
$ggmlCpu   = Join-Path $llamaDir "ggml-cpu.dll"
$ggmlDll   = Join-Path $llamaDir "ggml.dll"
$ggmlVulkan = Join-Path $llamaDir "ggml-vulkan.dll"
$benchExe = Join-Path $llamaDir "llama-bench.exe"

$model     = Join-Path $modelsDir "Meta-Llama-3.1-8B-Instruct-Q4_K_M.gguf"

$piperExe  = Join-Path $piperDir "piper.exe"
$voiceOnnx = Join-Path $voicesDir "en_US-lessac-small.onnx"
$voiceJson = Join-Path $voicesDir "en_US-lessac-small.onnx.json"

Write-Host "=== LocalTalker install test (Win64) ==="
Write-Host "ProjectRoot: $ProjectRoot"
Write-Host ""

foreach ($f in @($libllama,$ggmlBase,$ggmlCpu,$ggmlDll,$model,$piperExe,$voiceOnnx,$voiceJson)) {
  if (!(Test-Path $f)) { Fail "Missing required file: $f" }
  $len = (Get-Item $f).Length
  if ($len -le 0) { Fail "Empty file: $f" }
}
Ok "All required files exist"

if (Test-Path $ggmlVulkan) {
  Ok "Found ggml-vulkan.dll (Vulkan backend present)"
} else {
  Write-Host "WARN: ggml-vulkan.dll not found. Vulkan GPU offload will not be available." -ForegroundColor Yellow
}

# 1) Verify libllama loads and has key exports (and check GPU support)
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class Win32 {
  [DllImport("kernel32", CharSet=CharSet.Unicode, SetLastError=true)]
  public static extern IntPtr LoadLibraryExW(string lpFileName, IntPtr hFile, uint dwFlags);
  [DllImport("kernel32", SetLastError=true)]
  public static extern IntPtr GetProcAddress(IntPtr hModule, string lpProcName);
  [DllImport("kernel32", SetLastError=true)]
  public static extern bool FreeLibrary(IntPtr hModule);
}

[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
public delegate byte llama_supports_gpu_offload_fn();

[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
public delegate IntPtr llama_print_system_info_fn();

[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
public delegate void llama_backend_init_fn();

[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
public delegate void llama_backend_free_fn();

public static class LlamaHelpers {
  public static void BackendInit(IntPtr h) {
    var p = Win32.GetProcAddress(h, "llama_backend_init");
    if (p == IntPtr.Zero) return;
    var del = (llama_backend_init_fn)Marshal.GetDelegateForFunctionPointer(p, typeof(llama_backend_init_fn));
    del();
  }
  public static void BackendFree(IntPtr h) {
    var p = Win32.GetProcAddress(h, "llama_backend_free");
    if (p == IntPtr.Zero) return;
    var del = (llama_backend_free_fn)Marshal.GetDelegateForFunctionPointer(p, typeof(llama_backend_free_fn));
    del();
  }
  public static bool SupportsGpuOffload(IntPtr h) {
    var p = Win32.GetProcAddress(h, "llama_supports_gpu_offload");
    if (p == IntPtr.Zero) return false;
    var del = (llama_supports_gpu_offload_fn)Marshal.GetDelegateForFunctionPointer(p, typeof(llama_supports_gpu_offload_fn));
    return del() != 0;
  }
  public static string SystemInfo(IntPtr h) {
    var p = Win32.GetProcAddress(h, "llama_print_system_info");
    if (p == IntPtr.Zero) return "";
    var del = (llama_print_system_info_fn)Marshal.GetDelegateForFunctionPointer(p, typeof(llama_print_system_info_fn));
    var s = del();
    if (s == IntPtr.Zero) return "";
    return Marshal.PtrToStringAnsi(s) ?? "";
  }
}

// Llama API wrappers for text generation
[StructLayout(LayoutKind.Sequential)]
public struct LlamaModelParams {
  public int n_gpu_layers;
  public int main_gpu;
  public IntPtr tensor_split;
  public byte use_mmap;
  public byte use_mlock;
  public IntPtr progress_callback;
  public IntPtr progress_callback_user_data;
}

[StructLayout(LayoutKind.Sequential)]
public struct LlamaContextParams {
  public uint n_ctx;
  public uint n_batch;
  public int n_threads;
  public int n_threads_batch;
  public int n_threads_parallel;
  public int n_predict;
  public int n_keep;
  public int n_draft;
  public int n_chunks;
  public int n_ctx_min;
  public int n_ctx_max;
  public int n_ctx_exceeded;
  public int n_parallel;
  public int n_sequences;
  public float rope_freq_base;
  public float rope_freq_scale;
  public float yarn_ext_factor;
  public float yarn_attn_factor;
  public float yarn_beta_fast;
  public float yarn_beta_slow;
  public int yarn_orig_ctx;
  public byte rope_scaling_type;
  public byte pooling_type;
  public byte defrag_thold;
  public byte cb_eval;
  public byte cb_eval_user_data;
  public byte type_k;
  public byte type_v;
  public byte mul_mat_q;
  public byte logits_all;
  public byte embedding;
  public byte offload_kqv;
  public IntPtr abort_callback;
  public IntPtr abort_callback_user_data;
}

[StructLayout(LayoutKind.Sequential)]
public struct LlamaBatch {
  public int n_tokens;
  public IntPtr token;
  public IntPtr embd;
  public IntPtr pos;
  public IntPtr n_seq_id;
  public IntPtr seq_id;
  public byte logits;
}

[StructLayout(LayoutKind.Sequential)]
public struct LlamaSamplerChainParams {
  public byte reserved;
}

public static class LlamaApi {
  private static IntPtr hLib = IntPtr.Zero;
  
  public static IntPtr LoadLibrary(string dllPath) {
    uint LOAD_WITH_ALTERED_SEARCH_PATH = 0x00000008;
    hLib = Win32.LoadLibraryExW(dllPath, IntPtr.Zero, LOAD_WITH_ALTERED_SEARCH_PATH);
    return hLib;
  }
  
  public static void FreeLibrary() {
    if (hLib != IntPtr.Zero) {
      Win32.FreeLibrary(hLib);
      hLib = IntPtr.Zero;
    }
  }
  
  public static T GetFunction<T>(string name) where T : class {
    if (hLib == IntPtr.Zero) throw new InvalidOperationException("Library not loaded");
    IntPtr p = Win32.GetProcAddress(hLib, name);
    if (p == IntPtr.Zero) throw new DllNotFoundException("Function " + name + " not found");
    return Marshal.GetDelegateForFunctionPointer<T>(p);
  }
  
  public static LlamaModelParams ModelDefaultParams() {
    var fn = GetFunction<Func<LlamaModelParams>>("llama_model_default_params");
    return fn();
  }
  
  public static LlamaContextParams ContextDefaultParams() {
    var fn = GetFunction<Func<LlamaContextParams>>("llama_context_default_params");
    return fn();
  }
  
  public static IntPtr ModelLoadFromFile(string path, LlamaModelParams @params) {
    var fn = GetFunction<Func<string, LlamaModelParams, IntPtr>>("llama_model_load_from_file");
    return fn(path, @params);
  }
  
  public static void ModelFree(IntPtr model) {
    var fn = GetFunction<Action<IntPtr>>("llama_model_free");
    fn(model);
  }
  
  public static IntPtr ModelGetVocab(IntPtr model) {
    var fn = GetFunction<Func<IntPtr, IntPtr>>("llama_model_get_vocab");
    return fn(model);
  }
  
  public static IntPtr InitFromModel(IntPtr model, LlamaContextParams @params) {
    var fn = GetFunction<Func<IntPtr, LlamaContextParams, IntPtr>>("llama_init_from_model");
    if (fn == null) {
      fn = GetFunction<Func<IntPtr, LlamaContextParams, IntPtr>>("llama_new_context_with_model");
    }
    return fn(model, @params);
  }
  
  public static void Free(IntPtr ctx) {
    var fn = GetFunction<Action<IntPtr>>("llama_free");
    fn(ctx);
  }
  
  public static int Tokenize(IntPtr vocab, string text, int[] tokens, int maxTokens, bool addBos, bool parseSpecial) {
    var fn = GetFunction<Func<IntPtr, string, int, int[], int, bool, bool, int>>("llama_tokenize");
    return fn(vocab, text, text.Length, tokens, maxTokens, addBos, parseSpecial);
  }
  
  public static int Detokenize(IntPtr vocab, int[] tokens, int count, StringBuilder buffer, int bufferSize, bool addBos, bool special) {
    var fn = GetFunction<Func<IntPtr, int[], int, StringBuilder, int, bool, bool, int>>("llama_detokenize");
    return fn(vocab, tokens, count, buffer, bufferSize, addBos, special);
  }
  
  public static LlamaBatch BatchGetOne(int[] tokens, int count) {
    var fn = GetFunction<Func<int[], int, LlamaBatch>>("llama_batch_get_one");
    return fn(tokens, count);
  }
  
  public static int Decode(IntPtr ctx, LlamaBatch batch) {
    var fn = GetFunction<Func<IntPtr, LlamaBatch, int>>("llama_decode");
    return fn(ctx, batch);
  }
  
  public static bool VocabIsEog(IntPtr vocab, int token) {
    var fn = GetFunction<Func<IntPtr, int, bool>>("llama_vocab_is_eog");
    return fn(vocab, token);
  }
  
  public static IntPtr SamplerChainInit(LlamaSamplerChainParams @params) {
    var fn = GetFunction<Func<LlamaSamplerChainParams, IntPtr>>("llama_sampler_chain_init");
    return fn(@params);
  }
  
  public static void SamplerFree(IntPtr sampler) {
    var fn = GetFunction<Action<IntPtr>>("llama_sampler_free");
    fn(sampler);
  }
  
  public static IntPtr SamplerInitTopK(int k) {
    var fn = GetFunction<Func<int, IntPtr>>("llama_sampler_init_top_k");
    return fn(k);
  }
  
  public static IntPtr SamplerInitTopP(float p, int minKeep) {
    var fn = GetFunction<Func<float, int, IntPtr>>("llama_sampler_init_top_p");
    return fn(p, minKeep);
  }
  
  public static IntPtr SamplerInitTemp(float temp) {
    var fn = GetFunction<Func<float, IntPtr>>("llama_sampler_init_temp");
    return fn(temp);
  }
  
  public static IntPtr SamplerInitDist(uint seed) {
    var fn = GetFunction<Func<uint, IntPtr>>("llama_sampler_init_dist");
    return fn(seed);
  }
  
  public static void SamplerChainAdd(IntPtr chain, IntPtr sampler) {
    var fn = GetFunction<Action<IntPtr, IntPtr>>("llama_sampler_chain_add");
    fn(chain, sampler);
  }
  
  public static int SamplerSample(IntPtr sampler, IntPtr ctx, int pos) {
    var fn = GetFunction<Func<IntPtr, IntPtr, int, int>>("llama_sampler_sample");
    return fn(sampler, ctx, pos);
  }
  
  public static void SamplerAccept(IntPtr sampler, int token) {
    var fn = GetFunction<Action<IntPtr, int>>("llama_sampler_accept");
    fn(sampler, token);
  }
}
"@

$LOAD_WITH_ALTERED_SEARCH_PATH = 0x00000008
$h = [Win32]::LoadLibraryExW($libllama, [IntPtr]::Zero, $LOAD_WITH_ALTERED_SEARCH_PATH)
if ($h -eq [IntPtr]::Zero) { Fail "LoadLibrary failed for libllama.dll: $libllama" }

$requiredSyms = @(
  "llama_backend_init",
  "llama_backend_free",
  "llama_model_load_from_file",
  "llama_model_free",
  "llama_init_from_model",
  "llama_model_get_vocab",
  "llama_tokenize",
  "llama_detokenize",
  "llama_batch_get_one",
  "llama_decode",
  "llama_sampler_chain_default_params",
  "llama_sampler_chain_init",
  "llama_sampler_chain_add",
  "llama_sampler_sample",
  "llama_sampler_accept",
  "llama_vocab_is_eog",
  "llama_supports_gpu_offload",
  "llama_print_system_info"
)

foreach ($s in $requiredSyms) {
  $p = [Win32]::GetProcAddress($h, $s)
  if ($p -eq [IntPtr]::Zero) {
    [Win32]::FreeLibrary($h) | Out-Null
    Fail "Missing export in libllama.dll: $s"
  }
}

[LlamaHelpers]::BackendInit($h)
$supportsGpu = [LlamaHelpers]::SupportsGpuOffload($h)
$sysInfo = [LlamaHelpers]::SystemInfo($h)
[LlamaHelpers]::BackendFree($h)

[Win32]::FreeLibrary($h) | Out-Null
Ok "libllama.dll loads and exports required symbols"

if ($supportsGpu) {
  Ok "llama.cpp reports GPU offload is supported"
} else {
  Write-Host "WARN: llama.cpp reports GPU offload is NOT supported (this libllama.dll is CPU-only, or GPU backends weren't built in)." -ForegroundColor Yellow
  Write-Host "      To enable Vulkan: replace libllama.dll with a Vulkan-enabled build and add ggml-vulkan.dll next to it." -ForegroundColor Yellow
}
# Suppress verbose system info output
# if (![string]::IsNullOrWhiteSpace($sysInfo)) {
#   Write-Host "--- llama_print_system_info ---"
#   Write-Host $sysInfo
#   Write-Host "------------------------------"
# }

# 1b) Optional: run llama-bench to confirm Vulkan backend actually initializes
if (Test-Path $benchExe) {
  $oldEap = $ErrorActionPreference
  $ErrorActionPreference = "Continue"
  try {
    $benchCmd = "`"$benchExe`" -m `"$model`" -ngl 99 -n 16 -t 2 2>&1 | Out-Null"
    $null = cmd.exe /c $benchCmd
    $benchOut = (cmd.exe /c "`"$benchExe`" -m `"$model`" -ngl 99 -n 16 -t 2 2>&1" | Out-String)
  } finally {
    $ErrorActionPreference = $oldEap
  }
  if ($benchOut -match "loaded Vulkan backend" -or $benchOut -match "ggml_vulkan:" -or $benchOut -match "\|\s*Vulkan\s*\|") {
    Ok "Vulkan backend verified"
  }
} else {
  Write-Host "WARN: llama-bench.exe not found; skipping Vulkan runtime verification." -ForegroundColor Yellow
}

# 2) Piper smoke test
$outDir = Join-Path $ProjectRoot "Saved\\LocalTalkerTest"
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
$wav = Join-Path $outDir ("piper_test_{0}.wav" -f ([DateTimeOffset]::Now.ToUnixTimeSeconds()))

# Use cmd piping to feed stdin reliably (suppress Piper verbose output)
$cmd = "echo $Text| ""$piperExe"" -m ""$voiceOnnx"" -f ""$wav"" >nul 2>&1"
cmd.exe /c $cmd | Out-Null

if (!(Test-Path $wav)) { Fail "Piper did not create WAV: $wav" }
$wavSize = (Get-Item $wav).Length
if ($wavSize -lt 1000) { Fail "WAV too small ($wavSize bytes): $wav" }
Ok "Piper generated audio: $wav ($wavSize bytes)"

# 3) Bot conversation test using actual llama API calls
Write-Host ""
Write-Host "=== Bot Conversation Test (using llama.cpp API) ==="
Write-Host ""

try {
  # Load llama library
  [LlamaApi]::LoadLibrary($libllama) | Out-Null
  [LlamaHelpers]::BackendInit([LlamaApi]::hLib)
  
  # Load model
  $mParams = [LlamaApi]::ModelDefaultParams()
  $mParams.n_gpu_layers = 99  # Use GPU if available
  $mParams.use_mmap = 1
  $mParams.use_mlock = 0
  
  $llamaModel = [LlamaApi]::ModelLoadFromFile($model, $mParams)
  if ($llamaModel -eq [IntPtr]::Zero) {
    throw "Failed to load model"
  }
  Ok "Model loaded"
  
  $vocab = [LlamaApi]::ModelGetVocab($llamaModel)
  if ($vocab -eq [IntPtr]::Zero) {
    throw "Failed to get vocab"
  }
  
  # Create context
  $cParams = [LlamaApi]::ContextDefaultParams()
  $cParams.n_ctx = 2048
  $cParams.n_batch = 512
  $cParams.n_threads = 4
  $cParams.n_threads_batch = 4
  
  $ctx = [LlamaApi]::InitFromModel($llamaModel, $cParams)
  if ($ctx -eq [IntPtr]::Zero) {
    throw "Failed to create context"
  }
  Ok "Context created"
  
  # Define conversation
  $miloSystem = "You are Milo, a friendly and curious robot. Keep responses short (1-2 sentences)."
  $otisSystem = "You are Otis, a thoughtful and helpful robot. Keep responses short (1-2 sentences)."
  
  $conversation = @(
    @{Speaker="Milo"; Prompt="Hello Otis! How are you doing today?"},
    @{Speaker="Otis"; Prompt="Hello Milo! I'm doing well, thank you for asking. How about you?"},
    @{Speaker="Milo"; Prompt="I'm great! I've been thinking about the weather. Do you like sunny days?"},
    @{Speaker="Otis"; Prompt="I do enjoy sunny days! They make everything feel brighter. What's your favorite weather?"}
  )
  
  $audioFiles = @()
  $timestamp = [DateTimeOffset]::Now.ToUnixTimeSeconds()
  $conversationHistory = ""
  
  foreach ($turn in $conversation) {
    $speaker = $turn.Speaker
    $prompt = $turn.Prompt
    
    Write-Host "[$speaker]: $prompt"
    
    # Build full prompt
    $systemMsg = if ($speaker -eq "Milo") { $miloSystem } else { $otisSystem }
    $fullPrompt = "$systemMsg`n`n$conversationHistory`nUser: $prompt`nAssistant:"
    
    # Tokenize prompt
    $maxTokens = 4096
    $promptTokens = New-Object int[] $maxTokens
    $tokenCount = [LlamaApi]::Tokenize($vocab, $fullPrompt, $promptTokens, $maxTokens, $true, $true)
    
    if ($tokenCount -lt 0) {
      Write-Host "WARN: Tokenization failed or buffer too small" -ForegroundColor Yellow
      $response = "I'm processing that thought."
    } else {
      # Decode prompt tokens
      $batchSize = 512
      $promptIdx = 0
      while ($promptIdx -lt $tokenCount) {
        $chunkSize = [Math]::Min($batchSize, $tokenCount - $promptIdx)
        $chunkTokens = New-Object int[] $chunkSize
        [Array]::Copy($promptTokens, $promptIdx, $chunkTokens, 0, $chunkSize)
        
        $batch = [LlamaApi]::BatchGetOne($chunkTokens, $chunkSize)
        $decodeRes = [LlamaApi]::Decode($ctx, $batch)
        if ($decodeRes -ne 0) {
          throw "Decode failed: $decodeRes"
        }
        $promptIdx += $chunkSize
      }
      
      # Create sampler
      $sParams = New-Object LlamaSamplerChainParams
      $sampler = [LlamaApi]::SamplerChainInit($sParams)
      
      $topK = [LlamaApi]::SamplerInitTopK(40)
      [LlamaApi]::SamplerChainAdd($sampler, $topK)
      
      $topP = [LlamaApi]::SamplerInitTopP(0.95, 1)
      [LlamaApi]::SamplerChainAdd($sampler, $topP)
      
      $temp = [LlamaApi]::SamplerInitTemp(0.7)
      [LlamaApi]::SamplerChainAdd($sampler, $temp)
      
      $seed = [uint32](Get-Random -Minimum 1 -Maximum 999999)
      $dist = [LlamaApi]::SamplerInitDist($seed)
      [LlamaApi]::SamplerChainAdd($sampler, $dist)
      
      # Generate response
      $response = ""
      $maxGenTokens = 64
      
      for ($i = 0; $i -lt $maxGenTokens; $i++) {
        $token = [LlamaApi]::SamplerSample($sampler, $ctx, -1)
        [LlamaApi]::SamplerAccept($sampler, $token)
        
        if ([LlamaApi]::VocabIsEog($vocab, $token)) {
          break
        }
        
        # Detokenize
        $sb = New-Object System.Text.StringBuilder 256
        $detokRes = [LlamaApi]::Detokenize($vocab, @($token), 1, $sb, 256, $false, $false)
        if ($detokRes -gt 0) {
          $piece = $sb.ToString()
          $response += $piece
        }
        
        # Decode token
        $nextBatch = [LlamaApi]::BatchGetOne(@($token), 1)
        $decodeRes = [LlamaApi]::Decode($ctx, $nextBatch)
        if ($decodeRes -ne 0) {
          break
        }
      }
      
      [LlamaApi]::SamplerFree($sampler)
      $response = $response.Trim()
    }
    
    if ([string]::IsNullOrWhiteSpace($response)) {
      $response = "I'm thinking about that."
    }
    
    Write-Host "[$speaker] says: $response"
    
    # Update conversation history
    $conversationHistory += "User: $prompt`nAssistant: $response`n`n"
    
    # Generate audio (suppress Piper verbose output)
    $audioFile = Join-Path $outDir "conversation_${timestamp}_$speaker.wav"
    $piperCmd = "echo $response| ""$piperExe"" -m ""$voiceOnnx"" -f ""$audioFile"" >nul 2>&1"
    cmd.exe /c $piperCmd | Out-Null
    
    if (Test-Path $audioFile) {
      $audioFiles += $audioFile
      Ok "Generated audio for $speaker"
    } else {
      Write-Host "WARN: Failed to generate audio for $speaker" -ForegroundColor Yellow
    }
    
    Write-Host ""
    Start-Sleep -Milliseconds 500
  }
  
  # Cleanup
  [LlamaApi]::Free($ctx)
  [LlamaApi]::ModelFree($llamaModel)
  [LlamaHelpers]::BackendFree([LlamaApi]::hLib)
  [LlamaApi]::FreeLibrary()
  
} catch {
  Write-Host "ERROR in conversation test: $_" -ForegroundColor Red
  if ([LlamaApi]::hLib -ne [IntPtr]::Zero) {
    try { [LlamaApi]::FreeLibrary() } catch {}
  }
}

# 4) Play all audio files in sequence
Write-Host "=== Playing Conversation ==="
Write-Host ""

if ($audioFiles.Count -gt 0) {
  Ok "Playing $($audioFiles.Count) audio files in sequence..."
  
  foreach ($audioFile in $audioFiles) {
    if (Test-Path $audioFile) {
      try {
        $wmp = New-Object -ComObject WMPlayer.OCX
        $wmp.URL = $audioFile
        $wmp.controls.play()
        
        $fileSize = (Get-Item $audioFile).Length
        $estimatedDuration = [Math]::Max(2, ($fileSize / 10000))
        Start-Sleep -Seconds $estimatedDuration
        
        $wmp.controls.stop()
        [System.Runtime.Interopservices.Marshal]::ReleaseComObject($wmp) | Out-Null
      } catch {
        # Suppress playback errors
      }
    }
  }
  
  Ok "Conversation playback complete!"
} else {
  Write-Host "WARN: No audio files were generated for playback." -ForegroundColor Yellow
}

Write-Host ""
Ok "LocalTalker test completed. Audio files: $outDir"
exit 0
