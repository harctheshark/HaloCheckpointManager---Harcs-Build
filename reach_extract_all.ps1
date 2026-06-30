# Batch extractor: every scenario_structure_bsp under levels\{solo,multi,firefight,dlc} -> per-BSP .rinst blob,
# mirroring the tag path (reach_instanced\<tagrelpath>.rinst). Loads ManagedBlam ONCE. Zone-set-aware loading at
# runtime picks the loaded BSPs' blobs by tag name.
param(
  [string]$OutRoot = "G:\SteamLibrary\steamapps\common\HREK\reach_blobs"
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

function ECount($b){ $e=$b.Elements; return [int]$e.GetType().GetProperty('Count').GetValue($e) }
function EItem($b,$i){ $e=$b.Elements; $p=$e.GetType().GetProperty('Item'); Write-Output -NoEnumerate ($p.GetValue($e,[object[]]@([int]$i))) }
function U16($d){ $x=[int]$d; if($x -lt 0){$x+=65536}; return $x }
function FD($el,$n){ $el.SelectField($n).Data }
function VPoint($el){ return $el.Fields[0].Data }

function ExtractBsp($tagRel, $outPath){
  $tp=$tTP.GetMethod('FromPathAndExtension').Invoke($null,@($tagRel,"scenario_structure_bsp"))
  $tag=[Activator]::CreateInstance($tTF)
  try {
  $tTF.GetMethod('Load',[Type[]]@($tTP)).Invoke($tag,@($tp))
  $defs=$tag.SelectField("Struct:resource interface[0]/Block:raw_resources[0]/Struct:raw_items[0]/Block:instanced geometries definitions")
  $nDef=ECount $defs
  $defTris=@{}
  for($di=0;$di -lt $nDef;$di++){ $d=EItem $defs $di
    $base="Struct:collision info[0]"
    $sb=$null; try{ $sb=$d.SelectField("$base/Block:surfaces") }catch{}
    if($sb -eq $null){ continue }
    $ns=ECount $sb; if($ns -le 0){ continue }
    $eb=$d.SelectField("$base/Block:edges"); $vb=$d.SelectField("$base/Block:vertices")
    $ne=ECount $eb; $nv=ECount $vb
    $V=New-Object 'double[][]' $nv
    for($vi=0;$vi -lt $nv;$vi++){ $pt=VPoint (EItem $vb $vi); $V[$vi]=@([double]$pt[0],[double]$pt[1],[double]$pt[2]) }
    $E=New-Object 'int[][]' $ne
    for($ei=0;$ei -lt $ne;$ei++){ $ee=EItem $eb $ei
      $E[$ei]=@((U16 (FD $ee "start vertex")),(U16 (FD $ee "end vertex")),(U16 (FD $ee "forward edge")),(U16 (FD $ee "reverse edge")),(U16 (FD $ee "left surface")),(U16 (FD $ee "right surface"))) }
    $tris=New-Object System.Collections.ArrayList
    for($si=0;$si -lt $ns;$si++){ $sf=EItem $sb $si; $fe=U16 (FD $sf "first edge")
      $poly=New-Object System.Collections.ArrayList; $cur=$fe; $guard=0
      while($guard -lt 4096){ $ed=$E[$cur]
        if($ed[4] -eq $si){ [void]$poly.Add($ed[0]); $nx=$ed[2] } else { [void]$poly.Add($ed[1]); $nx=$ed[3] }
        if($nx -eq $fe){ break }; $cur=$nx; $guard++ }
      if($poly.Count -lt 3){ continue }
      for($k=1;$k -lt $poly.Count-1;$k++){ $a=$V[$poly[0]];$b=$V[$poly[$k]];$c=$V[$poly[$k+1]]
        [void]$tris.Add(@($a[0],$a[1],$a[2],$b[0],$b[1],$b[2],$c[0],$c[1],$c[2])) } }
    if($tris.Count -gt 0){ $defTris[$di]=$tris.ToArray() }
  }
  $inst=$tag.SelectField("instanced geometry instances"); $nInst=ECount $inst
  New-Item -ItemType Directory -Force (Split-Path $outPath) | Out-Null
  $fs=[System.IO.File]::Create($outPath); $bw=New-Object System.IO.BinaryWriter($fs)
  $bw.Write([byte[]][char[]]'RINST001')
  $countPos=$fs.Position; $bw.Write([int]0); $total=0
  for($ii=0;$ii -lt $nInst;$ii++){ $e=EItem $inst $ii
    $mi=[int](FD $e "ShortInteger:mesh_index"); if(-not $defTris.ContainsKey($mi)){ continue }
    $sc=[double](FD $e "scale"); if($sc -eq 0){ $sc=1.0 }
    $fw=FD $e "forward"; $lf=FD $e "left"; $up=FD $e "up"; $po=FD $e "position"
    $fx=[double]$fw[0]*$sc;$fy=[double]$fw[1]*$sc;$fz=[double]$fw[2]*$sc
    $lx=[double]$lf[0]*$sc;$ly=[double]$lf[1]*$sc;$lz=[double]$lf[2]*$sc
    $ux=[double]$up[0]*$sc;$uy=[double]$up[1]*$sc;$uz=[double]$up[2]*$sc
    $px=[double]$po[0];$py=[double]$po[1];$pz=[double]$po[2]
    foreach($t in $defTris[$mi]){
      for($v=0;$v -lt 9;$v+=3){ $vx=$t[$v];$vy=$t[$v+1];$vz=$t[$v+2]
        $bw.Write([single]($px + $fx*$vx + $lx*$vy + $ux*$vz))
        $bw.Write([single]($py + $fy*$vx + $ly*$vy + $uy*$vz))
        $bw.Write([single]($pz + $fz*$vx + $lz*$vy + $uz*$vz)) }
      $total++ } }
  $bw.Flush(); $fs.Position=$countPos; $bw.Write([int]$total); $bw.Flush(); $bw.Close()
  return @($nInst,$total)
  } finally {
    # ManagedBlam requires explicit disposal before GC, or the process aborts.
    try { $m=$tTF.GetMethod('Dispose',[Type[]]@()); if($m){ $m.Invoke($tag,@()) } } catch {}
  }
}

$roots=@("solo","multi","firefight","dlc")
$all=@()
foreach($r in $roots){ $p="$hrek\tags\levels\$r"; if(Test-Path $p){ $all += Get-ChildItem -Path $p -Recurse -Filter *.scenario_structure_bsp -File -ErrorAction SilentlyContinue } }
Write-Host "Found $($all.Count) BSP tags. Extracting to $OutRoot ..."
$i=0; $ok=0; $fail=0
foreach($f in $all){ $i++
  $rel=$f.FullName.Replace("$hrek\tags\",'').Replace('.scenario_structure_bsp','')
  $out=Join-Path $OutRoot ($rel + '.rinst')
  try {
    $r=ExtractBsp $rel $out
    $kb=[int]((Get-Item $out).Length/1KB)
    Write-Host ("[{0,3}/{1}] {2}  inst={3} tris={4} ({5} KB)" -f $i,$all.Count,$rel,$r[0],$r[1],$kb)
    $ok++
  } catch {
    Write-Host ("[{0,3}/{1}] {2}  FAILED: {3}" -f $i,$all.Count,$rel,$_.Exception.Message)
    $fail++
  }
}
Write-Host "DONE. ok=$ok fail=$fail  -> $OutRoot"
