<?php
// Datei, in der der Log-Buffer gespeichert wird
$logFile = "log/" . date("y-m-d-H-i-s") . "_log.txt";


// Rohdaten des Log-Buffers aus der Anfrage lesen
$inputData = file_get_contents("php://input");

// Struktur des Log-Eintrags definieren (entsprechend der C++-Struktur)
$logEntrySize = 3 * 4; // 3 Felder, je 4 Byte (unsigned long, int)
$logEntries = [];

// Statuscode-Beschreibungen
function getStatusDescription($statusCode) {
    switch ($statusCode) {
    	case 1013: return "clear_data_array()";
        case 1001: return "setup()";
        case 1005: return "call_backend()";
        case 1006: return "store_meter_value()";
        case 1008: return "WiFi returned";
        case 1009: return "WiFi lost";
        case 1010: return "Taf 7 meter reading trigger";
        case 1002: return "Taf 6 meter reading trigger";
        case 1014: return "Taf 7-900s meter reading trigger";
        case 1015: return "not enough heap to store value";
        case 1016: return "Buffer full, cannot store non-override value";    
        case 1011: return "Taf 14 meter reading trigger";
        case 1012: return "call backend trigger";
        case 1019: return "Sending Log";
        case 1020: return "Sending Log successful";
        case 1021: return "call_backend successful";
        case 1200: return "meter value <= 0"; 
        case 1201: return "current Meter value = previous meter value";
        case 1203: return "Suffix Must not be 0";
        case 1204: return "prefix suffix not correct";
        case 1205: return "Error Buffer Size Exceeded";
        case 3000: return "Complete Telegram received";
        case 3001: return "Telegram Pufferueberlauf";
        case 3002: return "Telegram timeout";
        case 3003: return "Telegramm zu groß für Speicher";
        case 4000: return "Connection to server failed (Cert!?)";
        case 7000: return "Stopping Wifi, Backendcall unsuccessfull";
        case 7001: return "Restarting Wifi";
        case 8000: return "Spiffs not mounted";
        case 8001: return "Fehler beim Öffnen der Zertifikatsdatei!";
        case 8002: return "Zertifikat gespeichert";
        case 8003: return "Fehler beim Öffnen der Cert Datei";
        case 8004: return "Kein Zertifikat erhalten!";
		default: return "Unknown status code";
    }
}


// Binärdaten in lesbares Format umwandeln
for ($i = 0; $i < strlen($inputData); $i += $logEntrySize) {
    $entryData = substr($inputData, $i, $logEntrySize);
    if (strlen($entryData) === $logEntrySize) {
        $timestamp = unpack("L", substr($entryData, 0, 4))[1]; // unsigned long
        $uptime = unpack("L", substr($entryData, 4, 4))[1];    // unsigned long
        $statusCode = unpack("l", substr($entryData, 8, 4))[1]; // int
        $logEntries[] = [
            'timestamp' => date("Y-m-d H:i:s", $timestamp),
            'uptime' => $uptime,
            'statusCode' => $statusCode,
			'description' => getStatusDescription($statusCode),
        ];
    }
}

// Log-Einträge in menschenlesbarem Format speichern
$logContent = "Timestamp\tUptime (s)\tStatus Code\n";
$logContent .= "-----------------------------------------\n";

// Extrahiere die Timestamps in ein separates Array
$timestamps = array_column($logEntries, 'timestamp');

// Sortiere nach Timestamp absteigend, aber stabil
array_multisort($timestamps, SORT_DESC, SORT_STRING, $logEntries);

foreach ($logEntries as $entry) {
    $logContent .= $entry['timestamp'] . "\t" . $entry['uptime'] . "\t\t" . $entry['statusCode'] . "\t\t" . $entry['description'] . "\n";
}
// Log-Daten in die Datei schreiben
if (file_put_contents($logFile, $logContent)) {
    http_response_code(200);
    echo "Log buffer saved successfully.";
} else {
    http_response_code(500);
    echo "Failed to save log buffer.";
}
?>