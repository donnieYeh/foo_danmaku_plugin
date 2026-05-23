# Elevated deploy: copies both DLLs into foobar2000\components and logs result.
$log = "$env:USERPROFILE\foo_danmaku_deploy.log"
"" | Out-File $log
function L($m) { $m | Out-File -Append $log }

try {
    Stop-Process -Name foobar2000 -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2

    $src1 = "C:\Users\donnie\projects\ai\windsurf\foobar_comment_flow\foo_danmaku\build\x64\foo_danmaku.dll"
    $src2 = "C:\Users\donnie\projects\ai\windsurf\netease_client\build\x64\netease_client.dll"
    $dst1 = "C:\Program Files\foobar2000\components\foo_danmaku.dll"
    $dst2 = "C:\Program Files\foobar2000\components\netease_client.dll"

    L "SRC1 $src1 $((Get-Item $src1).LastWriteTime)"
    L "SRC2 $src2 $((Get-Item $src2).LastWriteTime)"

    Copy-Item -Force -ErrorAction Stop $src1 $dst1
    L "OK foo_danmaku.dll -> $((Get-Item $dst1).LastWriteTime)"

    Copy-Item -Force -ErrorAction Stop $src2 $dst2
    L "OK netease_client.dll -> $((Get-Item $dst2).LastWriteTime)"
} catch {
    L "FAIL: $($_.Exception.Message)"
}
