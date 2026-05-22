# curl_test.ps1 — probe the NetEase weapi directly with Invoke-WebRequest
$headers = @{
    "Content-Type"    = "application/x-www-form-urlencoded"
    "Accept"          = "*/*"
    "Accept-Language" = "zh-CN,zh;q=0.9"
    "Referer"         = "https://music.163.com/"
    "Origin"          = "https://music.163.com"
    "User-Agent"      = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36"
}

Write-Host "=== TEST 1: weapi with dummy params (expect 200 empty) ==="
foreach ($url in @(
    "https://music.163.com/weapi/cloudsearch/pc",
    "https://music.163.com/weapi/search/get"
)) {
    Write-Host "  POST $url"
    try {
        $r = Invoke-WebRequest -Uri $url -Method POST -Headers $headers `
            -Body "params=AAAAAA&encSecKey=BBBBBB" -UseBasicParsing -TimeoutSec 10
        Write-Host "  HTTP $($r.StatusCode)  bytes=$($r.RawContentLength)"
        if ($r.RawContentLength -gt 0) {
            Write-Host "  $([System.Text.Encoding]::UTF8.GetString($r.Content).Substring(0,200))"
        }
    } catch { Write-Host "  ERROR: $($_.Exception.Message)" }
}

Write-Host ""
Write-Host "=== TEST 2: plain /api/ without encryption ==="
$plainTests = @(
    @{ url="https://music.163.com/api/cloudsearch/pc";      body="s=%E6%99%B4%E5%A4%A9&type=1&limit=5&offset=0&total=true" },
    @{ url="https://music.163.com/api/search/get";           body="s=%E6%99%B4%E5%A4%A9&type=1&limit=5&offset=0&total=true" },
    @{ url="https://music.163.com/api/v1/resource/comments/R_SO_4_/186016"; body="limit=5&offset=0&total=true" }
)
foreach ($t in $plainTests) {
    Write-Host "  POST $($t.url)"
    try {
        $r = Invoke-WebRequest -Uri $t.url -Method POST -Headers $headers `
            -Body $t.body -UseBasicParsing -TimeoutSec 10
        $text = [System.Text.Encoding]::UTF8.GetString($r.Content)
        Write-Host "  HTTP $($r.StatusCode)  bytes=$($r.RawContentLength)"
        Write-Host "  $($text.Substring(0,[Math]::Min(250,$text.Length)))"
    } catch { Write-Host "  ERROR: $($_.Exception.Message)" }
    Write-Host ""
}
