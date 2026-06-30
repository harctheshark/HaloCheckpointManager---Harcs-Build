$ErrorActionPreference='Stop'
$hrek='G:\SteamLibrary\steamapps\common\HREK'
[Environment]::CurrentDirectory="$hrek\bin"
[System.AppDomain]::CurrentDomain.add_AssemblyResolve({ param($s,$e)
  $n=($e.Name -split ',')[0]
  $l=[AppDomain]::CurrentDomain.GetAssemblies()|Where-Object{$_.GetName().Name -eq $n}|Select-Object -First 1
  if($l){return $l}; $p="$using:hrek\bin\$n.dll"; if(Test-Path $p){return [Reflection.Assembly]::LoadFrom($p)}; return $null })
$asm=[Reflection.Assembly]::LoadFrom("$hrek\bin\ManagedBlam.dll")
$tSys=$asm.GetType('Bungie.ManagedBlamSystem'); $tCb=$asm.GetType('Bungie.ManagedBlamCrashCallback')
$tSp=$asm.GetType('Bungie.ManagedBlamStartupParameters'); $tInit=$asm.GetType('Bungie.InitializationType')
$sys=[Activator]::CreateInstance($tSys); $sp=[Activator]::CreateInstance($tSp)
$tSp.GetProperty('InitializationLevel').SetValue($sp,[Enum]::Parse($tInit,'TagsOnly'))
$cb={param($i)} -as $tCb
$tSys.GetMethod('Start',[Type[]]@([string],$tCb,$tSp)).Invoke($sys,@($hrek,$cb,$sp))
$tTP=$asm.GetType('Bungie.Tags.TagPath'); $tTF=$asm.GetType('Bungie.Tags.TagFile')
$tp=$tTP.GetMethod('FromPathAndExtension').Invoke($null,@("levels\solo\m50\m50_000","scenario_structure_bsp"))
$tag=[Activator]::CreateInstance($tTF)
$tTF.GetMethod('Load',[Type[]]@($tTP)).Invoke($tag,@($tp))
Write-Host "Loaded ok=$($tp.IsTagFileAccessible())"

# reflection-based collection access (pwsh auto-enumeration breaks on Bungie collections)
function ECount($blk){ $e=$blk.Elements; return [int]$e.GetType().GetProperty('Count').GetValue($e) }
function EItem($blk,$i){ $e=$blk.Elements; $p=$e.GetType().GetProperty('Item'); Write-Output -NoEnumerate ($p.GetValue($e,[object[]]@([int]$i))) }
function FD($o,$n){ try{ $f=$o.SelectField($n); if($f -eq $null){return $null}; return $f.Data }catch{ return $null } }
function Vs($v){ if($v -eq $null){return '-'}; try{ return (@($v)|ForEach-Object{'{0:0.###}' -f $_}) -join ',' }catch{ return "$v" } }

$inst=$tag.SelectField("instanced geometry instances")
$cnt=ECount $inst
Write-Host "=== INSTANCES count=$cnt ==="
for($i=0;$i -lt [Math]::Min(6,$cnt);$i++){ $e=EItem $inst $i
  Write-Host ("[{0}] mesh={1} scale={2} pos=({3}) fwd=({4}) left=({5}) up=({6})" -f $i,(FD $e "ShortInteger:mesh_index"),(FD $e "scale"),(Vs (FD $e "position")),(Vs (FD $e "forward")),(Vs (FD $e "left")),(Vs (FD $e "up"))) }
$minx=1e9;$maxx=-1e9;$miny=1e9;$maxy=-1e9;$minz=1e9;$maxz=-1e9
for($i=0;$i -lt $cnt;$i++){ $p=FD (EItem $inst $i) "position"; if($p){ if($p[0]-lt$minx){$minx=$p[0]};if($p[0]-gt$maxx){$maxx=$p[0]};if($p[1]-lt$miny){$miny=$p[1]};if($p[1]-gt$maxy){$maxy=$p[1]};if($p[2]-lt$minz){$minz=$p[2]};if($p[2]-gt$maxz){$maxz=$p[2]} } }
Write-Host ("pos range: X[{0:0.#}..{1:0.#}] Y[{2:0.#}..{3:0.#}] Z[{4:0.#}..{5:0.#}]" -f $minx,$maxx,$miny,$maxy,$minz,$maxz)
$defs=$tag.SelectField("Struct:resource interface[0]/Block:raw_resources[0]/Struct:raw_items[0]/Block:instanced geometries definitions")
$dc=ECount $defs
Write-Host "=== DEFINITIONS count=$dc ==="
if($dc -gt 0){ $d0=EItem $defs 0
  Write-Host ("def0 meshIdx={0} compIdx={1}" -f (FD $d0 "mesh index"),(FD $d0 "compression index"))
  try{ $cs=$d0.SelectField("Struct:collision info[0]/Block:surfaces"); Write-Host "def0 collision surfaces=$(ECount $cs)" }catch{ Write-Host "coll fail: $($_.Exception.Message)" }
  # find first definition WITH collision surfaces, dump its structure
  for($di=0; $di -lt $dc; $di++){ $d=EItem $defs $di
    $cs=$null; try{ $cs=$d.SelectField("Struct:collision info[0]/Block:surfaces") }catch{}
    if($cs -ne $null){ $sc=ECount $cs; if($sc -gt 0){
      Write-Host ("--- def[{0}] meshIdx={1} HAS collision: surfaces={2} ---" -f $di,(FD $d "mesh index"),$sc)
      $base="Struct:collision info[0]"
      foreach($bn in @("Block:surfaces","Block:edges","Block:vertices","Block:planes","Block:bsp3d nodes","Block:leaves")){
        $b=$null; try{ $b=$d.SelectField("$base/$bn") }catch{}
        if($b -ne $null){ Write-Host ("    {0} count={1}" -f $bn,(ECount $b)) } }
      # dump first 4 collision vertices (point3 each)
      $vb=$d.SelectField("$base/Block:vertices"); $vc=ECount $vb
      Write-Host "  first collision vertices:"
      for($vi=0;$vi -lt [Math]::Min(4,$vc);$vi++){ $v=EItem $vb $vi
        $pos=$null; foreach($vn in @("point","position","Point")){ $pos=FD $v $vn; if($pos -ne $null){break} }
        if($pos -eq $null){ $vf=$v.Fields; $vfp=$vf.GetType().GetProperty('Item'); $f0=$vfp.GetValue($vf,[object[]]@([int]0)); $pos=$f0.Data }
        Write-Host ("    v[{0}] = ({1})" -f $vi,(Vs $pos)) }
      break } } }
}
$sys.Dispose(); Write-Host "DONE"
