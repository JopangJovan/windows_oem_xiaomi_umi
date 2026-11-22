@echo off
title Xiaomi Mi 10 (umi) Sensor Check
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "Start-Service SensorDataService -EA SilentlyContinue;" ^
  "$out = @();" ^
  "$out += '=== PnP Sensor Devices ===';" ^
  "$out += (Get-PnpDevice -Class Sensor -EA SilentlyContinue | Where-Object {$_.Status -eq 'OK'} | Format-Table Status,FriendlyName,InstanceId -AutoSize | Out-String);" ^
  "$out += '=== Sensor Services ===';" ^
  "$out += (Get-Service SensrSvc,SensorDataService,SensorService -EA SilentlyContinue | Format-Table Name,Status,StartType -AutoSize | Out-String);" ^
  "$out += '=== AutoRotation Registry ===';" ^
  "$out += (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\AutoRotation' -EA SilentlyContinue | Out-String);" ^
  "$out += '=== WinRT Live Sensor Readings ===';" ^
  "$a = [Windows.Devices.Sensors.Accelerometer,Windows.Devices.Sensors,ContentType=WindowsRuntime]::GetDefault();" ^
  "if ($a) { $r = $a.GetCurrentReading(); $out += ('Accelerometer: X=' + [math]::Round($r.AccelerationX,3) + ' Y=' + [math]::Round($r.AccelerationY,3) + ' Z=' + [math]::Round($r.AccelerationZ,3)) } else { $out += 'Accelerometer: NOT FOUND' };" ^
  "$g = [Windows.Devices.Sensors.Gyrometer,Windows.Devices.Sensors,ContentType=WindowsRuntime]::GetDefault();" ^
  "if ($g) { $r = $g.GetCurrentReading(); $out += ('Gyrometer:     X=' + [math]::Round($r.AngularVelocityX,3) + ' Y=' + [math]::Round($r.AngularVelocityY,3) + ' Z=' + [math]::Round($r.AngularVelocityZ,3)) } else { $out += 'Gyrometer:     NOT FOUND' };" ^
  "$m = [Windows.Devices.Sensors.Magnetometer,Windows.Devices.Sensors,ContentType=WindowsRuntime]::GetDefault();" ^
  "if ($m) { $r = $m.GetCurrentReading(); $out += ('Magnetometer:  X=' + [math]::Round($r.MagneticFieldX,2) + ' Y=' + [math]::Round($r.MagneticFieldY,2) + ' Z=' + [math]::Round($r.MagneticFieldZ,2)) } else { $out += 'Magnetometer:  NOT FOUND' };" ^
  "$l = [Windows.Devices.Sensors.LightSensor,Windows.Devices.Sensors,ContentType=WindowsRuntime]::GetDefault();" ^
  "if ($l) { $r = $l.GetCurrentReading(); $out += ('LightSensor:   ' + [math]::Round($r.IlluminanceInLux,1) + ' lux') } else { $out += 'LightSensor:   NOT FOUND' };" ^
  "$b = [Windows.Devices.Sensors.Barometer,Windows.Devices.Sensors,ContentType=WindowsRuntime]::GetDefault();" ^
  "if ($b) { $r = $b.GetCurrentReading(); $out += ('Barometer:     ' + [math]::Round($r.StationPressureInHectopascals,2) + ' hPa') } else { $out += 'Barometer:     NOT FOUND' };" ^
  "$o = [Windows.Devices.Sensors.SimpleOrientationSensor,Windows.Devices.Sensors,ContentType=WindowsRuntime]::GetDefault();" ^
  "if ($o) { $out += ('Orientation:   ' + $o.GetCurrentOrientation()) } else { $out += 'Orientation:   NOT FOUND' };" ^
  "$text = $out -join \"`r`n\"; Write-Output $text; Set-Content -Path 'C:\Windows\Temp\sensor-check.txt' -Value $text"
echo.
echo Saved to C:\Windows\Temp\sensor-check.txt
pause
