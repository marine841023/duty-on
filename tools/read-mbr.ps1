$ErrorActionPreference = 'Stop'
$out = 'd:\src\traeSprite\mbr_dump.txt'
$lines = @()
foreach ($n in 3,4) {
    try {
        $fs = [IO.FileStream]::new("\\.\PhysicalDrive$n",[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read -bor [IO.FileShare]::Write)
        $buf = New-Object byte[] 512
        [void]$fs.Read($buf,0,512)
        $fs.Close()
        $sig = [BitConverter]::ToUInt32($buf,510)
        $lines += "=== Disk $n: signature 0x{0:X4}" -f $sig
        for($i=0;$i -lt 4;$i++){
            $o = 446 + $i*16
            if ($buf[$o] -eq 0 -and $buf[$o+4] -eq 0) { continue }
            $type = $buf[$o+4]
            $lba = [BitConverter]::ToUInt32($buf,$o+8)
            $cnt = [BitConverter]::ToUInt32($buf,$o+12)
            $lines += ("Part{0}: boot={1} type=0x{2:X2} startLBA={3} sectors={4} ({5:N1} MB)" -f ($i+1),$buf[$o],$type,$lba,$cnt,($cnt*512/1MB))
        }
    } catch {
        $lines += "=== Disk $n: ERROR $($_.Exception.Message)"
    }
}
[IO.File]::WriteAllLines($out, $lines)
