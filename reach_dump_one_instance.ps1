# Dump source instance transform (forward/left/up/position/scale) near a target world pos, to compare vs runtime.
param(
  [string]$TagRel = "levels\solo\m50\m50_000",
  [double]$Tx = 399.1, [double]$Ty = 343.3, [double]$Tz = 136.5, [double]$R = 12.0
)
$ErrorActionPreference='Stop'
$hrek='G:\SteamLibrary\steamapps\common\HREK'
[Environment]::CurrentDirectory="$hrek\bin"
[System.AppDomain]::CurrentDomain.add_AssemblyResolve({ param($s,$e)
  $n=($e.Name -split ',')[0]; $l=[AppDomain]::CurrentDomain.GetAssemblies()|Where-Object{$_.GetName().Name -eq $n}|Select-Object -First 1
  if($l){return $l}; $p="$using:hrek\bin\$n.dll"; if(Test-Path $p){return [Reflection.Assembly]::LoadFrom($p)}; return $null })
$asm=[Reflection.Assembly]::LoadFrom("$hrek\bin\ManagedBlam.dll")
$tSys=$asm.GetType('Bungie.ManagedBlamSystem'); $tCb=$asm.GetType('Bungie.ManagedBlamCrashCallback')
$tSp=$asm.GetType('Bungie.ManagedBlamStartupParameters'); $tInit=$asm.GetType('Bungie.InitializationType')
$sys=[Activator]::CreateInstance($tSys); $sp=[Activator]::CreateInstance($tSp)
$tSp.GetProperty('InitializationLevel').SetValue($sp,[Enum]::Parse($tInit,'TagsOnly'))
$cb={param($i)} -as $tCb
$tSys.GetMethod('Start',[Type[]]@([string],$tCb,$tSp)).Invoke($sys,@($hrek,$cb,$sp))
$tTP=$asm.GetType('Bungie.Tags.TagPath'); $tTF=$asm.GetType('Bungie.Tags.TagFile')
$tp=$tTP.GetMethod('FromPathAndExtension').Invoke($null,@($TagRel,"scenario_structure_bsp"))
$tag=[Activator]::CreateInstance($tTF); $tTF.GetMethod('Load',[Type[]]@($tTP)).Invoke($tag,@($tp))
Write-Host "Loaded $TagRel ok=$($tp.IsTagFileAccessible())"

function ECount($b){ $e=$b.Elements; return [int]$e.GetType().GetProperty('Count').GetValue($e) }
function EItem($b,$i){ $e=$b.Elements; $p=$e.GetType().GetProperty('Item'); Write-Output -NoEnumerate ($p.GetValue($e,[object[]]@([int]$i))) }
function FD($el,$n){ $el.SelectField($n).Data }

$inst=$tag.SelectField("instanced geometry instances"); $n=ECount $inst
Write-Host "total instances: $n  | searching near ($Tx,$Ty,$Tz) r=$R"
$hits=0
for($i=0;$i -lt $n -and $hits -lt 8;$i++){ $e=EItem $inst $i
  $po=FD $e "position"; $dx=[double]$po[0]-$Tx; $dy=[double]$po[1]-$Ty; $dz=[double]$po[2]-$Tz
  if(($dx*$dx+$dy*$dy+$dz*$dz) -le ($R*$R)){
    $fw=FD $e "forward"; $lf=FD $e "left"; $up=FD $e "up"; $sc=FD $e "scale"; $mi=FD $e "ShortInteger:mesh_index"
    Write-Host ("[inst {0}] mesh={1} scale={2:F3} pos=({3:F2},{4:F2},{5:F2})" -f $i,$mi,$sc,$po[0],$po[1],$po[2])
    Write-Host ("    forward=({0:F3},{1:F3},{2:F3})  left=({3:F3},{4:F3},{5:F3})  up=({6:F3},{7:F3},{8:F3})" -f `
      $fw[0],$fw[1],$fw[2],$lf[0],$lf[1],$lf[2],$up[0],$up[1],$up[2])
    $hits++ } }
Write-Host "hits=$hits"
Write-Host ""
Write-Host "RUNTIME A2CFE0 for pos=(399.1,343.3,136.5):"
Write-Host "  r0=(0.239,-0.071,0.969)  r1=(-0.934,-0.289,0.209)  r2=(0.265,-0.955,-0.135)"
