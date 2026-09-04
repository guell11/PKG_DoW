param(
    [int]$Port = 8765,
    [switch]$NoBrowser
)

$ErrorActionPreference = 'Stop'
$Root = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$Web = Join-Path $Root 'webui'
$Data = Join-Path $Root 'userdata'
$Logs = Join-Path $Root 'logs'
$LibraryFile = Join-Path $Data 'library.json'
$SettingsFile = Join-Path $Data 'settings.json'
New-Item -ItemType Directory -Force -Path $Data,$Logs | Out-Null

$DefaultSettings = [ordered]@{
    player_name = 'Player1'
    kyty_path = ''
    shadps4_path = ''
    performance_profile = 'balanced'
    present_mode = 'Fifo'
    shader_optimization = 'Performance'
    resolution = '1280x720'
    fullscreen = $false
    gpu_index = -1
    vblank_frequency = 60
    redzone = $true
    playgo_hack = $false
    ps4_boot_mode = 'auto'
}
$ProfileOverrides = @{
    stable   = @{ present_mode='Fifo'; shader_optimization='None'; resolution='1280x720'; vblank_frequency=60; redzone=$true }
    balanced = @{ present_mode='Fifo'; shader_optimization='Performance'; resolution='1280x720'; vblank_frequency=60; redzone=$true }
    turbo    = @{ present_mode='Mailbox'; shader_optimization='Performance'; resolution='960x540'; vblank_frequency=60; redzone=$true }
}
$script:Processes = @{}

function Read-JsonFile([string]$Path, $Fallback) {
    if (-not (Test-Path -LiteralPath $Path)) { return $Fallback }
    try {
        $text = [System.IO.File]::ReadAllText($Path, [System.Text.Encoding]::UTF8)
        if ([string]::IsNullOrWhiteSpace($text)) { return $Fallback }
        return ($text | ConvertFrom-Json)
    } catch { return $Fallback }
}

function Save-JsonFile([string]$Path, $Value) {
    $json = ConvertTo-Json -InputObject $Value -Depth 12
    $tmp = "$Path.tmp"
    [System.IO.File]::WriteAllText($tmp, $json, (New-Object System.Text.UTF8Encoding -ArgumentList $false))
    Move-Item -LiteralPath $tmp -Destination $Path -Force
}

function Get-Settings {
    $saved = Read-JsonFile $SettingsFile $null
    $merged = [ordered]@{}
    foreach ($key in $DefaultSettings.Keys) { $merged[$key] = $DefaultSettings[$key] }
    if ($null -ne $saved) {
        foreach ($p in $saved.PSObject.Properties) { $merged[$p.Name] = $p.Value }
    }
    return [pscustomobject]$merged
}

function Get-Library {
    $items = Read-JsonFile $LibraryFile @()
    if ($null -eq $items) { return @() }
    return @($items)
}

function Get-StableId([string]$Path) {
    $full = [System.IO.Path]::GetFullPath($Path).ToLowerInvariant()
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($full)
    $sha = [System.Security.Cryptography.SHA1]::Create()
    try { $hash = $sha.ComputeHash($bytes) } finally { $sha.Dispose() }
    return (-join ($hash[0..7] | ForEach-Object { $_.ToString('x2') }))
}

function Get-TextId([string]$Text) {
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($Text.ToLowerInvariant())
    $sha = [System.Security.Cryptography.SHA1]::Create()
    try { $hash = $sha.ComputeHash($bytes) } finally { $sha.Dispose() }
    return (-join ($hash[0..7] | ForEach-Object { $_.ToString('x2') }))
}

function Find-Engine([string]$Kind) {
    $cfg = Get-Settings
    $configured = if ($Kind -eq 'kyty') { [string]$cfg.kyty_path } else { [string]$cfg.shadps4_path }
    if ($configured -and (Test-Path -LiteralPath $configured -PathType Leaf)) {
        return [System.IO.Path]::GetFullPath($configured)
    }
    $names = if ($Kind -eq 'kyty') { @('kyty_emulator.exe','Kyty.exe') } else { @('shadPS4.exe') }
    $roots = @($Root, (Join-Path $Root 'bin'), (Join-Path $Root 'build'), (Join-Path $Root 'runtime'))
    foreach ($r in $roots) {
        if (-not (Test-Path -LiteralPath $r -PathType Container)) { continue }
        foreach ($name in $names) {
            $direct = Join-Path $r $name
            if (Test-Path -LiteralPath $direct -PathType Leaf) { return [System.IO.Path]::GetFullPath($direct) }
        }
    }
    return ''
}

function Get-CacheInfo {
    $dirs = @((Join-Path $Root '_PipelineCache'))
    $kyty = Find-Engine 'kyty'
    if ($kyty) { $dirs += (Join-Path (Split-Path -Parent $kyty) '_PipelineCache') }
    $seen = @{}
    $files = 0L; $size = 0L; $best = (Join-Path $Root '_PipelineCache')
    foreach ($dir in $dirs) {
        if (-not $dir) { continue }
        $full = [System.IO.Path]::GetFullPath($dir)
        if ($seen.ContainsKey($full)) { continue }
        $seen[$full] = $true
        if (Test-Path -LiteralPath $full -PathType Container) {
            $best = $full
            Get-ChildItem -LiteralPath $full -File -ErrorAction SilentlyContinue | ForEach-Object {
                $files++; $size += $_.Length
            }
        }
    }
    return [pscustomobject]@{ path=$best; files=$files; size=$size }
}

function Update-ProcessState {
    if ($script:Processes.Count -eq 0) { return }
    $items = Get-Library
    $changed = $false
    foreach ($id in @($script:Processes.Keys)) {
        $entry = $script:Processes[$id]
        $proc = Get-Process -Id ([int]$entry.pid) -ErrorAction SilentlyContinue
        if ($null -ne $proc) { continue }
        $elapsed = [Math]::Max(0, [int]([DateTimeOffset]::UtcNow.ToUnixTimeSeconds() - [int64]$entry.started))
        for ($i=0; $i -lt $items.Count; $i++) {
            if ([string]$items[$i].id -eq [string]$id) {
                $old = 0
                if ($items[$i].PSObject.Properties.Name -contains 'play_seconds') { $old = [int64]$items[$i].play_seconds }
                $items[$i].play_seconds = $old + $elapsed
                $items[$i].status = "Encerrado ($($entry.engine))"
                $changed = $true
                break
            }
        }
        $script:Processes.Remove($id)
    }
    if ($changed) { Save-JsonFile $LibraryFile @($items) }
}

function Get-GpuArchitecture([string]$Name) {
    $n = ([string]$Name).ToUpperInvariant()
    if ($n -match 'RTX 50') { return 'NVIDIA Blackwell' }
    if ($n -match 'RTX 40') { return 'NVIDIA Ada Lovelace' }
    if ($n -match 'RTX 30') { return 'NVIDIA Ampere' }
    if ($n -match 'RTX 20|GTX 16') { return 'NVIDIA Turing' }
    if ($n -match 'RADEON.*RX 7') { return 'AMD RDNA 3' }
    if ($n -match 'RADEON.*RX 6') { return 'AMD RDNA 2' }
    if ($n -match 'NVIDIA') { return 'NVIDIA Vulkan' }
    if ($n -match 'AMD|RADEON') { return 'AMD Vulkan' }
    if ($n -match 'INTEL') { return 'Intel Vulkan' }
    return 'Generic Vulkan'
}

function Get-VulkanProbe {
    $loader = Join-Path $env:WINDIR 'System32\vulkan-1.dll'
    $available = Test-Path -LiteralPath $loader
    $gpus = New-Object System.Collections.ArrayList
    $vkinfo = Get-Command vulkaninfo.exe -ErrorAction SilentlyContinue
    if ($vkinfo) {
        try {
            $text = (& $vkinfo.Source --summary 2>&1 | Out-String)
            $matches = [regex]::Matches($text, 'GPU(?<idx>\d+):[\s\S]*?deviceName\s*=\s*(?<name>[^\r\n]+)')
            foreach ($m in $matches) {
                $gpuName=$m.Groups['name'].Value.Trim(); $arch=Get-GpuArchitecture $gpuName
                [void]$gpus.Add([pscustomobject]@{index=[int]$m.Groups['idx'].Value;name=$gpuName;type='GPU';architecture=$arch;api_version='';api_version_raw=0;driver_version=0;vendor_id=0;device_id=0;vram=0;tuning=[pscustomobject]@{mode='adaptive';target_ratio=0;target=0;descriptor_cache_entries=4096;pipeline_compile='background-safe';upload_policy='no-stall-ring+spill';barrier_policy='read-read-elision'};translation_path='Prospero/RDNA2 -> IR -> SPIR-V -> Vulkan -> driver nativo';swapchain=$true;push_descriptor=$false;memory_budget=$false;memory_priority=$false;pipeline_cache_control=$false;descriptor_buffer=$false;shadps4_ready=$false})
            }
        } catch {}
    }
    return [pscustomobject]@{available=$available;loader_version='';gpus=@($gpus);error=$(if($available){''}else{'vulkan-1.dll não encontrado'})}
}

function Get-SystemStatus {
    Update-ProcessState
    $driveRoot = [System.IO.Path]::GetPathRoot($Root)
    $drive = Get-CimInstance Win32_LogicalDisk -Filter "DeviceID='$($driveRoot.TrimEnd('\'))'" -ErrorAction SilentlyContinue
    $os = Get-CimInstance Win32_OperatingSystem -ErrorAction SilentlyContinue
    $ramTotal = if ($os) { [int64]$os.TotalVisibleMemorySize * 1024 } else { 0L }
    $ramFree = if ($os) { [int64]$os.FreePhysicalMemory * 1024 } else { 0L }
    $active = [ordered]@{}
    foreach ($id in $script:Processes.Keys) { $active[$id] = $script:Processes[$id] }
    $cache = Get-CacheInfo
    $vk = Get-VulkanProbe
    return [pscustomobject]@{
        ok = $true
        os = [Environment]::OSVersion.VersionString
        cpu_name = [Environment]::GetEnvironmentVariable('PROCESSOR_IDENTIFIER')
        cpu_threads = [Environment]::ProcessorCount
        ram_total = $ramTotal
        ram_used = [Math]::Max(0L, $ramTotal-$ramFree)
        disk_total = if ($drive) { [int64]$drive.Size } else { 0L }
        disk_used = if ($drive) { [int64]($drive.Size-$drive.FreeSpace) } else { 0L }
        vulkan_runtime = [bool]$vk.available
        vulkan = $vk
        engines = [pscustomobject]@{ kyty=(Find-Engine 'kyty'); shadps4=(Find-Engine 'shadps4') }
        pipeline_cache = $cache
        active_processes = [pscustomobject]$active
        launcher = 'PowerShell fallback'
        server_version = 'PKG_DoW/2.3'
    }
}

function Get-Diagnostics {
    $s = Get-SystemStatus
    $free = [int64]$s.disk_total - [int64]$s.disk_used
    $checks = @(
        [pscustomobject]@{name='PowerShell';ok=$true;detail=$PSVersionTable.PSVersion.ToString()},
        [pscustomobject]@{name='Runtime Vulkan';ok=[bool]$s.vulkan_runtime;detail='vulkan-1.dll / driver GPU'},
        [pscustomobject]@{name='Core PS5';ok=[bool]$s.engines.kyty;detail=($(if($s.engines.kyty){$s.engines.kyty}else{'Configure kyty_emulator.exe'}))},
        [pscustomobject]@{name='Core PS4';ok=[bool]$s.engines.shadps4;detail=($(if($s.engines.shadps4){$s.engines.shadps4}else{'Opcional: configure shadPS4.exe'}))},
        [pscustomobject]@{name='RAM >= 16 GB';ok=([int64]$s.ram_total -ge 16GB);detail=("{0:N1} GB detectados" -f ([int64]$s.ram_total/1GB))},
        [pscustomobject]@{name='Disco livre >= 10 GB';ok=($free -ge 10GB);detail=("{0:N1} GB livres" -f ($free/1GB))}
    )
    return [pscustomobject]@{ok=$true;checks=$checks;status=$s}
}

function Read-ParamSfo([string]$Path) {
    $out = @{}
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $out }
    try {
        [byte[]]$b = [System.IO.File]::ReadAllBytes($Path)
        if ($b.Length -lt 20 -or $b[0] -ne 0 -or $b[1] -ne 0x50 -or $b[2] -ne 0x53 -or $b[3] -ne 0x46) { return $out }
        $keyOff = [BitConverter]::ToUInt32($b, 8)
        $dataOff = [BitConverter]::ToUInt32($b, 12)
        $count = [BitConverter]::ToUInt32($b, 16)
        for ($i=0; $i -lt $count; $i++) {
            $off = 20 + 16*$i; if ($off + 16 -gt $b.Length) { break }
            $keyIdx = [BitConverter]::ToUInt16($b, $off)
            $fmt = [BitConverter]::ToUInt16($b, $off+2)
            $len = [BitConverter]::ToUInt32($b, $off+4)
            $maxLen = [BitConverter]::ToUInt32($b, $off+8)
            $valueOff = [BitConverter]::ToUInt32($b, $off+12)
            $keyPos = [int]($keyOff + $keyIdx); if ($keyPos -ge $b.Length) { continue }
            $keyEnd = $keyPos; while ($keyEnd -lt $b.Length -and $b[$keyEnd] -ne 0) { $keyEnd++ }
            $key = [Text.Encoding]::UTF8.GetString($b, $keyPos, $keyEnd-$keyPos)
            $pos = [int]($dataOff + $valueOff); if ($pos -ge $b.Length) { continue }
            $take = [Math]::Min([int][Math]::Min($len,$maxLen), $b.Length-$pos)
            if ($fmt -eq 0x0404 -and $take -ge 4) { $out[$key] = [BitConverter]::ToUInt32($b,$pos) }
            elseif ($take -gt 0) {
                $text = [Text.Encoding]::UTF8.GetString($b,$pos,$take).Trim([char]0).Trim()
                if ($text) { $out[$key] = $text }
            }
        }
    } catch {}
    return $out
}

function Get-ContentRegion([string]$ContentId) {
    if ([string]::IsNullOrWhiteSpace($ContentId) -or $ContentId.Length -lt 2) { return '' }
    switch ($ContentId.Substring(0,2).ToUpperInvariant()) { 'UP' {'Américas'} 'EP' {'Europa'} 'JP' {'Japão'} 'HP' {'Ásia'} 'KP' {'Coreia'} default {''} }
}

function Get-GameDescriptor([string]$RawPath) {
    if ([string]::IsNullOrWhiteSpace($RawPath)) { throw 'Caminho vazio.' }
    $item = Get-Item -LiteralPath $RawPath -ErrorAction Stop
    $path = $item.FullName
    $isFolder = $item.PSIsContainer
    $kind = if ($isFolder) {'folder'} elseif ($item.Extension -ieq '.pkg') {'pkg'} elseif ($item.Name -ieq 'eboot.bin' -or $item.Extension -ieq '.elf' -or $item.Extension -ieq '.bin') {'elf'} else {'file'}
    $name = if ($isFolder) {$item.Name} else {$item.BaseName}
    $meta = @{}
    if ($isFolder) {
        $sfo = Join-Path $path 'sce_sys\param.sfo'; if (-not (Test-Path -LiteralPath $sfo)) { $sfo = Join-Path $path 'param.sfo' }
        $meta = Read-ParamSfo $sfo
    } elseif ($item.Name -ieq 'eboot.bin' -or $item.Extension -ieq '.elf' -or $item.Extension -ieq '.bin') {
        $sfo = Join-Path $item.DirectoryName 'sce_sys\param.sfo'; if (-not (Test-Path -LiteralPath $sfo)) { $sfo = Join-Path $item.DirectoryName 'param.sfo' }
        $meta = Read-ParamSfo $sfo
    }
    if ($meta.ContainsKey('TITLE') -and [string]$meta['TITLE']) { $name = [string]$meta['TITLE'] }
    $titleId = if ($meta.ContainsKey('TITLE_ID')) {[string]$meta['TITLE_ID']} else {''}
    if (-not $titleId) { $m = [regex]::Match(($path + ' ' + $name).ToUpperInvariant(), '(?:CUSA|PPSA|LAPY|NPXS|NPXX)\d{5}'); if ($m.Success) { $titleId = $m.Value } }
    $titleId = $titleId.ToUpperInvariant()
    $contentId = if ($meta.ContainsKey('CONTENT_ID')) {[string]$meta['CONTENT_ID']} else {''}
    $appVer = if ($meta.ContainsKey('APP_VER')) {[string]$meta['APP_VER']} elseif ($meta.ContainsKey('VERSION')) {[string]$meta['VERSION']} else {''}
    $category = if ($meta.ContainsKey('CATEGORY')) {[string]$meta['CATEGORY']} else {''}
    $gameId = if ($titleId -match '^CUSA\d{5}$') { Get-TextId ("ps4:" + $titleId) } else { Get-StableId $path }
    $platform = 'Auto'
    $probe = ($titleId + ' ' + $name).ToUpperInvariant()
    if ($probe -match 'PPSA|PS5') { $platform='PS5' }
    elseif ($probe -match 'CUSA|LAPY|PS4') { $platform='PS4' }
    elseif ($kind -eq 'pkg') { $platform='PKG' }
    $now = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
    return [pscustomobject]@{
        id=$gameId; path=$path; title=$name; title_id=$titleId; content_id=$contentId;
        platform=$platform; kind=$kind; size=$(if($isFolder){0L}else{[int64]$item.Length});
        favorite=$false; last_played=0; play_seconds=0; added_at=$now; icon='';
        status=$(if($kind -eq 'pkg'){'Precisa instalar/extrair'}else{'Pronto'}); profile_override='global'; ps4_boot_mode='auto'; cusa=($titleId -match '^CUSA\d{5}$'); app_ver=$appVer; region=(Get-ContentRegion $contentId); system_ver=''; category=$category
    }
}

function Add-Game([string]$Path) {
    $game = Get-GameDescriptor $Path
    $items = Get-Library
    $out = New-Object System.Collections.ArrayList
    $replaced = $false
    foreach ($g in $items) {
        if ([string]$g.id -eq [string]$game.id) {
            if ($g.PSObject.Properties.Name -contains 'favorite') { $game.favorite = [bool]$g.favorite }
            if ($g.PSObject.Properties.Name -contains 'play_seconds') { $game.play_seconds = [int64]$g.play_seconds }
            if ($g.PSObject.Properties.Name -contains 'profile_override') { $game.profile_override = [string]$g.profile_override }
            if ($g.PSObject.Properties.Name -contains 'ps4_boot_mode') { $game.ps4_boot_mode = [string]$g.ps4_boot_mode }
            [void]$out.Add($game); $replaced=$true
        } else { [void]$out.Add($g) }
    }
    if (-not $replaced) { [void]$out.Add($game) }
    Save-JsonFile $LibraryFile @($out)
    return $game
}

function Scan-CusaRoot([string]$RootPath) {
    if (-not (Test-Path -LiteralPath $RootPath -PathType Container)) { throw 'Raiz CUSA inválida.' }
    $dirs = New-Object System.Collections.ArrayList
    $rootItem = Get-Item -LiteralPath $RootPath
    if ($rootItem.Name -match '^CUSA\d{5}$') { [void]$dirs.Add($rootItem.FullName) }
    Get-ChildItem -LiteralPath $RootPath -Directory -Recurse -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^CUSA\d{5}$' } | Select-Object -First 500 | ForEach-Object { [void]$dirs.Add($_.FullName) }
    $out = New-Object System.Collections.ArrayList
    foreach ($dir in ($dirs | Select-Object -Unique)) {
        if ((Test-Path -LiteralPath (Join-Path $dir 'eboot.bin')) -or (Test-Path -LiteralPath (Join-Path $dir 'sce_sys\param.sfo'))) {
            try { [void]$out.Add((Add-Game $dir)) } catch {}
        }
    }
    return @($out)
}

function Select-ImportPath([string]$Kind) {
    Add-Type -AssemblyName System.Windows.Forms
    if ($Kind -eq 'folder') {
        $dlg = New-Object System.Windows.Forms.FolderBrowserDialog
        $dlg.Description = 'Importar pasta de jogo extraido'
        if ($dlg.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { return $dlg.SelectedPath }
        return ''
    }
    $dlg = New-Object System.Windows.Forms.OpenFileDialog
    $dlg.Title = 'Importar jogo / PKG / ELF'
    $dlg.Filter = 'Jogos (*.pkg;*.elf;*.bin)|*.pkg;*.elf;*.bin|Todos os arquivos (*.*)|*.*'
    if ($dlg.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { return $dlg.FileName }
    return ''
}

function Join-WindowsArguments($Values) {
    $parts = foreach ($value in $Values) {
        $s = [string]$value
        if ($s -notmatch '[\s"]') { $s }
        else { '"' + $s.Replace('"','\"') + '"' }
    }
    return ($parts -join ' ')
}

function Start-Game([string]$Id) {
    $items = Get-Library
    $index = -1
    for ($i=0; $i -lt $items.Count; $i++) { if ([string]$items[$i].id -eq $Id) { $index=$i; break } }
    if ($index -lt 0) { throw 'Jogo não encontrado na biblioteca.' }
    $game = $items[$index]
    $path = [string]$game.path
    if (-not (Test-Path -LiteralPath $path)) { throw "Caminho não existe: $path" }
    if ([string]$game.kind -eq 'pkg') { throw 'PKG bruto não é alvo de boot. Instale/extraia no core correspondente e importe a pasta/eboot.bin.' }
    $cfg = Get-Settings
    $profileOverride = if ($game.PSObject.Properties.Name -contains 'profile_override') { [string]$game.profile_override } else { 'global' }
    if ($ProfileOverrides.ContainsKey($profileOverride)) {
        foreach ($k in $ProfileOverrides[$profileOverride].Keys) { $cfg.($k) = $ProfileOverrides[$profileOverride][$k] }
    }
    $platform = ([string]$game.platform).ToUpperInvariant()
    $args = New-Object System.Collections.Generic.List[string]
    if ($platform -eq 'PS4') {
        $exe = Find-Engine 'shadps4'
        if (-not $exe) { throw 'Core PS4 não configurado. Informe shadPS4.exe em Configurações.' }
        $mode = if ($game.PSObject.Properties.Name -contains 'ps4_boot_mode') {[string]$game.ps4_boot_mode} else {[string]$cfg.ps4_boot_mode}
        $titleId = ([string]$game.title_id).ToUpperInvariant()
        if ($mode -eq 'cusa' -and $titleId -match '^CUSA\d{5}$') {
            $args.Add($titleId)
        } else {
            $target = $path
            if (Test-Path -LiteralPath $path -PathType Container) {
                $target = Join-Path $path 'eboot.bin'
                if (-not (Test-Path -LiteralPath $target -PathType Leaf)) {
                    $found = Get-ChildItem -LiteralPath $path -Filter eboot.bin -File -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
                    if (-not $found) { throw 'Nenhum eboot.bin encontrado na pasta PS4.' }
                    $target = $found.FullName
                }
            }
            $args.Add($target)
        }
        if ([bool]$cfg.fullscreen) { $args.Add('--fullscreen'); $args.Add('true') }
        $engine='shadPS4'
    } else {
        $exe = Find-Engine 'kyty'
        if (-not $exe) { throw 'kyty_emulator.exe não foi encontrado. Compile/configure o core PS5.' }
        $args.Add('--game'); $args.Add($path)
        $args.Add('--present-mode'); $args.Add([string]$cfg.present_mode)
        $args.Add('--shader-optimization-type'); $args.Add([string]$cfg.shader_optimization)
        if ([string]$cfg.resolution -match '^(\d+)x(\d+)$') {
            $args.Add('--screen-width'); $args.Add($Matches[1]); $args.Add('--screen-height'); $args.Add($Matches[2])
        }
        $gpu = [int]$cfg.gpu_index; if ($gpu -ge 0) { $args.Add('--gpu'); $args.Add([string]$gpu) }
        $vblank = [Math]::Max(30,[Math]::Min(360,[int]$cfg.vblank_frequency))
        $args.Add('--vblank-frequency'); $args.Add([string]$vblank)
        $args.Add('--user-name'); $args.Add(([string]$cfg.player_name).Substring(0,[Math]::Min(16,([string]$cfg.player_name).Length)))
        if ([bool]$cfg.fullscreen) { $args.Add('--fullscreen') }
        if ([bool]$cfg.redzone) { $args.Add('--redzone') }
        if ([bool]$cfg.playgo_hack) { $args.Add('--playgo-hack') }
        $engine='KytyPS5'
    }
    $stamp = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
    $log = Join-Path $Logs "launch-$Id-$stamp.log"
    $err = "$log.err"
    $argLine = Join-WindowsArguments $args.ToArray()
    $proc = Start-Process -FilePath $exe -ArgumentList $argLine -WorkingDirectory (Split-Path -Parent $exe) -RedirectStandardOutput $log -RedirectStandardError $err -PassThru
    $script:Processes[$Id] = [pscustomobject]@{pid=$proc.Id;started=$stamp;engine=$engine}
    $game.last_played = $stamp; $game.status="Executando com $engine"
    if ($game.PSObject.Properties.Name -contains 'last_exit_code') { $game.last_exit_code=$null } else { $game | Add-Member -NotePropertyName last_exit_code -NotePropertyValue $null }
    $items[$index]=$game; Save-JsonFile $LibraryFile @($items)
    return [pscustomobject]@{ok=$true;pid=$proc.Id;engine=$engine;log=$log;args=$args.ToArray()}
}

function Open-LocalPath([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) { $Path=$Root }
    if (Test-Path -LiteralPath $Path -PathType Leaf) { $Path=Split-Path -Parent $Path }
    if (-not (Test-Path -LiteralPath $Path)) { throw "Caminho não existe: $Path" }
    Start-Process -FilePath $Path | Out-Null
}

function Get-Mime([string]$Path) {
    switch ([System.IO.Path]::GetExtension($Path).ToLowerInvariant()) {
        '.html' {'text/html; charset=utf-8'} '.css' {'text/css; charset=utf-8'} '.js' {'application/javascript; charset=utf-8'}
        '.json' {'application/json; charset=utf-8'} '.png' {'image/png'} '.jpg' {'image/jpeg'} '.jpeg' {'image/jpeg'}
        '.svg' {'image/svg+xml'} '.ico' {'image/x-icon'} default {'application/octet-stream'}
    }
}

function Send-Bytes($Context, [byte[]]$Bytes, [string]$ContentType='application/octet-stream', [int]$Status=200) {
    $r=$Context.Response; $r.StatusCode=$Status; $r.ContentType=$ContentType; $r.ContentLength64=$Bytes.Length
    $r.Headers['Cache-Control']='no-store'
    $r.OutputStream.Write($Bytes,0,$Bytes.Length); $r.OutputStream.Close()
}
function Send-Text($Context, [string]$Text, [string]$ContentType='text/plain; charset=utf-8', [int]$Status=200) {
    Send-Bytes $Context ([System.Text.Encoding]::UTF8.GetBytes($Text)) $ContentType $Status
}
function Send-Json($Context, $Value, [int]$Status=200) {
    $json = ConvertTo-Json -InputObject $Value -Depth 12 -Compress
    Send-Text $Context $json 'application/json; charset=utf-8' $Status
}
function Read-BodyJson($Context) {
    $reader = New-Object System.IO.StreamReader($Context.Request.InputStream, $Context.Request.ContentEncoding)
    try { $text=$reader.ReadToEnd() } finally { $reader.Dispose() }
    if ([string]::IsNullOrWhiteSpace($text)) { return [pscustomobject]@{} }
    return ($text | ConvertFrom-Json)
}

function Handle-Get($Context) {
    $u=$Context.Request.Url; $path=$u.AbsolutePath
    if ($path -eq '/api/status') { Send-Json $Context (Get-SystemStatus); return }
    if ($path -eq '/api/vulkan') { Send-Json $Context (Get-VulkanProbe); return }
    if ($path -eq '/api/cusa') { $games=@(Get-Library | Where-Object {[string]$_.title_id -match '^CUSA\d{5}$'}); Send-Json $Context ([pscustomobject]@{count=$games.Count;games=$games}); return }
    if ($path -eq '/api/diagnostics') { Send-Json $Context (Get-Diagnostics); return }
    if ($path -eq '/api/library') { Send-Json $Context @(Get-Library); return }
    if ($path -eq '/api/settings') { Send-Json $Context (Get-Settings); return }
    if ($path -eq '/api/logs') {
        $logs=@(Get-ChildItem -LiteralPath $Logs -File -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 30 | ForEach-Object {[pscustomobject]@{name=$_.Name;size=$_.Length;modified=[DateTimeOffset]$_.LastWriteTimeUtc}})
        Send-Json $Context $logs; return
    }
    if ($path -eq '/api/file') {
        $raw=$Context.Request.QueryString['path']; if (-not $raw) { throw 'Arquivo não informado.' }
        $file=[System.IO.Path]::GetFullPath($raw); if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw 'Arquivo não encontrado.' }
        Send-Bytes $Context ([System.IO.File]::ReadAllBytes($file)) (Get-Mime $file); return
    }
    $rel = if ($path -eq '/') {'index.html'} else {$path.TrimStart('/')}
    $rel=[Uri]::UnescapeDataString($rel).Replace('/',[System.IO.Path]::DirectorySeparatorChar)
    $file=[System.IO.Path]::GetFullPath((Join-Path $Web $rel))
    $webFull=[System.IO.Path]::GetFullPath($Web) + [System.IO.Path]::DirectorySeparatorChar
    if (-not $file.StartsWith($webFull,[System.StringComparison]::OrdinalIgnoreCase) -or -not (Test-Path -LiteralPath $file -PathType Leaf)) {
        Send-Json $Context ([pscustomobject]@{ok=$false;error='Não encontrado'}) 404; return
    }
    Send-Bytes $Context ([System.IO.File]::ReadAllBytes($file)) (Get-Mime $file)
}

function Handle-Post($Context) {
    $path=$Context.Request.Url.AbsolutePath; $body=Read-BodyJson $Context
    if ($path -eq '/api/import-path') { $g=Add-Game ([string]$body.path); Send-Json $Context ([pscustomobject]@{ok=$true;game=$g}); return }
    if ($path -eq '/api/import-dialog') {
        $value=Select-ImportPath ([string]$body.kind); if (-not $value) { Send-Json $Context ([pscustomobject]@{ok=$true;cancelled=$true}); return }
        $g=Add-Game $value; Send-Json $Context ([pscustomobject]@{ok=$true;game=$g}); return
    }
    if ($path -eq '/api/scan-cusa') { $value=Select-ImportPath 'folder'; if(-not $value){Send-Json $Context ([pscustomobject]@{ok=$false;cancelled=$true});return}; $games=@(Scan-CusaRoot $value); Send-Json $Context ([pscustomobject]@{ok=$true;path=$value;count=$games.Count;games=$games}); return }
    if ($path -eq '/api/launch') { Send-Json $Context (Start-Game ([string]$body.id)); return }
    if ($path -eq '/api/game-profile') {
        $profile=[string]$body.profile; if(@('global','stable','balanced','turbo') -notcontains $profile){throw 'Perfil de jogo inválido.'}
        $items=Get-Library; $game=$null
        foreach($g in $items){if([string]$g.id -eq [string]$body.id){if($g.PSObject.Properties.Name -contains 'profile_override'){$g.profile_override=$profile}else{$g|Add-Member -NotePropertyName profile_override -NotePropertyValue $profile};$game=$g;break}}
        if(-not $game){throw 'Jogo não encontrado.'}; Save-JsonFile $LibraryFile @($items); Send-Json $Context ([pscustomobject]@{ok=$true;game=$game}); return
    }
    if ($path -eq '/api/game-boot-mode') { $mode=[string]$body.mode; if(@('auto','eboot','cusa') -notcontains $mode){throw 'Modo PS4 inválido.'}; $items=Get-Library; $game=$null; foreach($g in $items){if([string]$g.id -eq [string]$body.id){if($g.PSObject.Properties.Name -contains 'ps4_boot_mode'){$g.ps4_boot_mode=$mode}else{$g|Add-Member -NotePropertyName ps4_boot_mode -NotePropertyValue $mode};$game=$g;break}}; if(-not $game){throw 'Jogo não encontrado.'}; Save-JsonFile $LibraryFile @($items); Send-Json $Context ([pscustomobject]@{ok=$true;game=$game}); return }
    if ($path -eq '/api/favorite') {
        $items=Get-Library; $game=$null
        foreach($g in $items){if([string]$g.id -eq [string]$body.id){$g.favorite=-not [bool]$g.favorite;$game=$g;break}}
        if(-not $game){throw 'Jogo não encontrado.'}; Save-JsonFile $LibraryFile @($items); Send-Json $Context ([pscustomobject]@{ok=$true;game=$game}); return
    }
    if ($path -eq '/api/remove') {
        $items=@(Get-Library | Where-Object {[string]$_.id -ne [string]$body.id}); Save-JsonFile $LibraryFile $items; Send-Json $Context ([pscustomobject]@{ok=$true}); return
    }
    if ($path -eq '/api/settings') {
        $cfg=Get-Settings; foreach($p in $body.PSObject.Properties){if($DefaultSettings.Contains($p.Name)){$cfg.($p.Name)=$p.Value}}
        if(@('Fifo','Mailbox','Immediate') -notcontains [string]$cfg.present_mode){$cfg.present_mode='Fifo'}
        if(@('None','Size','Performance') -notcontains [string]$cfg.shader_optimization){$cfg.shader_optimization='Performance'}
        if(@('stable','balanced','turbo','custom') -notcontains [string]$cfg.performance_profile){$cfg.performance_profile='balanced'}
        if(@('auto','eboot','cusa') -notcontains [string]$cfg.ps4_boot_mode){$cfg.ps4_boot_mode='auto'}
        $cfg.vblank_frequency=[Math]::Max(30,[Math]::Min(360,[int]$cfg.vblank_frequency)); $cfg.gpu_index=[Math]::Max(-1,[int]$cfg.gpu_index)
        Save-JsonFile $SettingsFile $cfg; Send-Json $Context ([pscustomobject]@{ok=$true;settings=$cfg}); return
    }
    if ($path -eq '/api/open-folder') { Open-LocalPath ([string]$body.path); Send-Json $Context ([pscustomobject]@{ok=$true}); return }
    if ($path -eq '/api/open-engine') {
        $exe=if([string]$body.engine -eq 'shadps4'){Find-Engine 'shadps4'}else{Find-Engine 'kyty'}; if(-not $exe){throw 'Engine não encontrada/configurada.'}
        Start-Process -FilePath $exe -WorkingDirectory (Split-Path -Parent $exe) | Out-Null; Send-Json $Context ([pscustomobject]@{ok=$true}); return
    }
    if ($path -eq '/api/open-pipeline-cache') { $cache=Get-CacheInfo; New-Item -ItemType Directory -Force -Path $cache.path | Out-Null; Open-LocalPath $cache.path; Send-Json $Context ([pscustomobject]@{ok=$true;path=$cache.path}); return }
    if ($path -eq '/api/verify') { $checked=@(Get-Library | ForEach-Object {[pscustomobject]@{id=$_.id;path=$_.path;exists=(Test-Path -LiteralPath ([string]$_.path))}}); Send-Json $Context ([pscustomobject]@{ok=$true;checked=$checked}); return }
    if ($path -eq '/api/clean-cache') { $cacheDir=Join-Path $Data 'cache';$removed=0L;if(Test-Path $cacheDir){Get-ChildItem $cacheDir -File -Recurse -ErrorAction SilentlyContinue|%{$removed+=$_.Length};Remove-Item $cacheDir -Force -Recurse};Send-Json $Context ([pscustomobject]@{ok=$true;removed=$removed});return }
    Send-Json $Context ([pscustomobject]@{ok=$false;error='Endpoint não encontrado'}) 404
}

$listener = New-Object System.Net.HttpListener
$bound=$false
for($p=$Port;$p -le $Port+10;$p++){
    try{$listener.Prefixes.Clear();$listener.Prefixes.Add("http://127.0.0.1:$p/");$listener.Start();$Port=$p;$bound=$true;break}catch{if($listener.IsListening){$listener.Stop()}}
}
if(-not $bound){throw "Não foi possível abrir uma porta local entre $Port e $($Port+10)."}
$url="http://127.0.0.1:$Port/"
Write-Host ''
Write-Host ' PKG_DoW 2.3 - UX local (PowerShell fallback)' -ForegroundColor Cyan
Write-Host " $url"
Write-Host ' Feche esta janela para encerrar a UX.'
Write-Host ''
if(-not $NoBrowser){Start-Process $url | Out-Null}
try{
    while($listener.IsListening){
        $ctx=$listener.GetContext()
        try{
            if($ctx.Request.HttpMethod -eq 'GET'){Handle-Get $ctx}
            elseif($ctx.Request.HttpMethod -eq 'POST'){Handle-Post $ctx}
            else{Send-Json $ctx ([pscustomobject]@{ok=$false;error='Método não suportado'}) 405}
        } catch {
            try{Send-Json $ctx ([pscustomobject]@{ok=$false;error=$_.Exception.Message}) 500}catch{}
        }
    }
} finally { if($listener.IsListening){$listener.Stop()};$listener.Close() }
