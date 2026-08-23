[CmdletBinding()]
param(
    [ValidateRange(1, 99)]
    [int]$Bots = 99,
    [ValidateRange(1, 120)]
    [int]$SendRate = 20,
    [ValidateRange(2, 60)]
    [int]$DurationSeconds = 10,
    [ValidateRange(1024, 65436)]
    [int]$BasePort = 27051,
    [string]$MapName = 'blender_feature_park',
    [uint32]$MapHash = 0
)

$ErrorActionPreference = 'Stop'
$ProtocolVersion = [uint16]11

function Get-Fnv1a32 {
    param([Parameter(Mandatory)][string]$Text)
    [uint32]$hash = 2166136261
    foreach ($value in [Text.Encoding]::UTF8.GetBytes($Text)) {
        $hash = $hash -bxor $value
        $hash = [uint32](
            ([uint64]$hash * 16777619) % [uint64]4294967296
        )
    }
    return $hash
}

function New-PosePacket {
    param(
        [uint32]$Role,
        [uint32]$Session,
        [uint32]$Sequence,
        [uint32]$MapHash,
        [uint64]$SenderTimeMicroseconds,
        [float]$X,
        [float]$Z
    )
    $stream = [IO.MemoryStream]::new(72)
    $writer = [IO.BinaryWriter]::new($stream)
    try {
        $writer.Write([uint32]0x504D334B)
        $writer.Write($ProtocolVersion)
        $writer.Write([uint16]72)
        $writer.Write($Role)
        $writer.Write($Session)
        $writer.Write($Sequence)
        $writer.Write($MapHash)
        $writer.Write($SenderTimeMicroseconds)
        $writer.Write($X)
        $writer.Write([float]0)
        $writer.Write($Z)
        $writer.Write([float]1)
        $writer.Write([float]0)
        $writer.Write([float]0)
        $writer.Write([float]0)
        $writer.Write([float]0)
        $writer.Write([float]1)
        $writer.Write([uint32]0)
        return $stream.ToArray()
    } finally {
        $writer.Dispose()
        $stream.Dispose()
    }
}

if ($MapHash -eq 0) {
    $MapHash = Get-Fnv1a32 -Text $MapName
}
$hostEndpoint = [Net.IPEndPoint]::new(
    [Net.IPAddress]::Loopback, $BasePort
)
$peers = [Collections.Generic.List[object]]::new()
try {
    foreach ($index in 0..($Bots - 1)) {
        $role = $index + 2
        $socket = [Net.Sockets.UdpClient]::new($BasePort + $role - 1)
        $socket.Client.Blocking = $false
        $peers.Add([pscustomobject]@{
            Role = [uint32]$role
            Session = [uint32](0x60000000 + $role)
            Sequence = [uint32]0
            Socket = $socket
            ReceivedPackets = [uint64]0
            ReceivedBytes = [uint64]0
            X = [float](($index % 10) * 12)
            Z = [float]([math]::Floor($index / 10) * 12)
        })
    }

    Write-Host ((
        'Sending {0} simulated peers to host 127.0.0.1:{1} for {2}s ' +
        'on map "{3}" (hash 0x{4:X8})...'
    ) -f $Bots, $BasePort, $DurationSeconds, $MapName, $MapHash)
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $nextSend = [double]0
    $interval = 1.0 / $SendRate
    [uint64]$sentPackets = 0
    [uint64]$sentBytes = 0
    while ($clock.Elapsed.TotalSeconds -lt $DurationSeconds) {
        $now = $clock.Elapsed.TotalSeconds
        if ($now -ge $nextSend) {
            $nextSend += $interval
            $senderTime = [uint64]($clock.ElapsedTicks * 1000000 /
                [Diagnostics.Stopwatch]::Frequency)
            foreach ($peer in $peers) {
                $peer.Sequence = [uint32]($peer.Sequence + 1)
                $packet = New-PosePacket `
                    -Role $peer.Role `
                    -Session $peer.Session `
                    -Sequence $peer.Sequence `
                    -MapHash $MapHash `
                    -SenderTimeMicroseconds $senderTime `
                    -X $peer.X `
                    -Z $peer.Z
                [void]$peer.Socket.Send(
                    $packet, $packet.Length, $hostEndpoint
                )
                ++$sentPackets
                $sentBytes += $packet.Length
            }
        }
        foreach ($peer in $peers) {
            while ($peer.Socket.Available -gt 0) {
                $sender = [Net.IPEndPoint]::new(
                    [Net.IPAddress]::Any, 0
                )
                try {
                    $received = $peer.Socket.Receive([ref]$sender)
                    ++$peer.ReceivedPackets
                    $peer.ReceivedBytes += $received.Length
                } catch [Net.Sockets.SocketException] {
                    if ($_.Exception.SocketErrorCode -ne
                        [Net.Sockets.SocketError]::WouldBlock) {
                        throw
                    }
                }
            }
        }
        Start-Sleep -Milliseconds 1
    }

    $receivedPackets = [uint64]((
        $peers | Measure-Object ReceivedPackets -Sum
    ).Sum)
    $receivedBytes = [uint64]((
        $peers | Measure-Object ReceivedBytes -Sum
    ).Sum)
    $seconds = [math]::Max($clock.Elapsed.TotalSeconds, 0.001)
    Write-Host ''
    Write-Host ('Sent to host: {0:N0} packets, {1:N2} Mbit/s' -f
        $sentPackets, (($sentBytes * 8) / $seconds / 1000000))
    Write-Host ('Received from host: {0:N0} packets, {1:N2} Mbit/s' -f
        $receivedPackets, (($receivedBytes * 8) / $seconds / 1000000))
    Write-Host (
        'Bots receiving host/relay traffic: {0}/{1}' -f
        (($peers | Where-Object ReceivedPackets -gt 0).Count), $Bots
    )
    Write-Host 'Inspect host telemetry for known peers, relay drops, and bytes.'
} finally {
    foreach ($peer in $peers) {
        $peer.Socket.Dispose()
    }
}
