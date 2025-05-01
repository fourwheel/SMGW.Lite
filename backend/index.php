<?php
#ini_set('display_errors', 1);
#ini_set('display_startup_errors', 1);
#error_reporting(E_ALL);

include("../v3/valid_clients.php");

// Eingangsdaten aus GET oder POST
$id = $_GET['ID'] ?? '';
$token = $_GET['token'] ?? '';

// Sicherheitsprüfung
if (!isset($valid_clients[$id]) || !hash_equals($valid_clients[$id], $token)) {
    http_response_code(403);
    echo "Zugriff verweigert.";
    exit;
}
$data = [];
$data["ID"] = "";
$data["ID"] = $_GET['ID'];
if(isset($_GET['backend_test']) && $_GET['backend_test'] == "true") 
{
	http_response_code(200);
	exit;
}

// Binärdaten auslesen
$rawData = file_get_contents('php://input');

// Anzahl der Einträge berechnen
$entrySize = 4 + 4 + 4 + 4; // timestamp (4 Bytes) + meter (4 Bytes) + temperature (4 Bytes) + solar (4)
$dataCount = strlen($rawData) / $entrySize;

if ($dataCount != floor($dataCount)) {
    http_response_code(400);
    echo "Invalid data length.";
    exit;
}

// Binärdaten in Einträge zerlegen
$value_count = 0;
for ($i = 0; $i < $dataCount; $i++) {
    $offset = $i * $entrySize;
    $timestamp = unpack("L", substr($rawData, $offset, 4))[1];
	$meter = unpack("L", substr($rawData, $offset + 4, 4))[1];
    $temperature = unpack("L", substr($rawData, $offset + 8, 4))[1];
	$solar = unpack("L", substr($rawData, $offset + 12, 4))[1];
	
	if($meter == 0) continue;
	#if($temperature > 200) $temperature = -3;
	$value_count++;
    $data["values"][] = [
        "timestamp" => $timestamp,
        "meter" => $meter,
        "temperature" => $temperature,
		"solar" => $solar,		
    ];
}

// Sortieren
usort($data["values"], function ($a, $b) {
    return $a["timestamp"] <=> $b["timestamp"];
});

http_response_code(200);
echo "Data received and processed successfully.";


if($value_count > 5) 
{
	// Erzeuge den Dateinamen basierend auf der aktuellen Zeit
	$filename = date("y-m-d-H-i-s") . ".txt";

	// Öffne die Datei zum Schreiben
	$file = fopen("log/".$filename, "w");
	if ($file === false) {
		die("Fehler beim Öffnen der Datei");
	}

	// Durchlaufe die Werte und schreibe sie zeilenweise in die Datei
	foreach ($data["values"] as $entry) {
		// Extrahiere die Werte
		$timestamp = $entry["timestamp"];
		$meter = $entry["meter"];
		$temperature = $entry["temperature"];
		$solar = $entry["solar"];

		// Erstelle die Zeile im gewünschten Format
		$line = "$timestamp;$meter;$temperature;$solar\n";

		// Schreibe die Zeile in die Datei
		fwrite($file, $line);
	}

	// Schließe die Datei
	fclose($file);
}


#exit;

$prev["timestamp"] = 0;
$prev["meter"] = 0;

#print_r($data);
include("../config.php");

$sql = "SELECT * FROM `sml_v1` WHERE `id` = '".$data["ID"]."' order by `timestamp_client` DESC LIMIT 1";
$result = mysqli_query($_link, $sql);
while($row = mysqli_fetch_array($result))
{
	$prev["meter"] = $row['meter_value'];
	$prev["timestamp"] = $row['timestamp_client'];
}
$r = 0;

foreach ($data["values"] as $item) {
	
	if(($item["timestamp"] - $prev["timestamp"]) == 0) continue;
	
	$item["power"] = (($item["meter"] - $prev["meter"])/10)/(($item["timestamp"] - $prev["timestamp"])/3600);
    $item["power"] = round($item["power"], 0);
	echo $item["timestamp"]." ".$item["meter"]." ".$item["power"]."\n";
	if($item["power"] == 0 || $item["power"] > 16000) continue;
	if($item['meter'] < 0 ) $item['meter'] = 0;
	if(!isset($item['temperature'])) $item['temperature'] = 0;
	$item['temperature'] = $item['temperature']/100;
	if(!isset($item['P_PV'])) $item['P_PV'] = 0;
	//if(!$item['power']) $item['power'] = 0;
	if(!isset($item['meter_value_PV'])) $item['meter_value_PV'] = 0;
	
	$sql4 = "INSERT INTO `sml_v1` (
	`i`, 
	`id`, 
	`timestamp_server`, 
	`timestamp_client`, 
	`meter_value`, 
	`meter_value_PV`, 
	`power_back`, 
	`power_back_PV`, 
	`temperature`) 
	VALUES (
	NULL, 
	'".$data['ID']."', 
	'".date('Y-m-d H:i:s', time())."', 
	'".$item["timestamp"]."', 
	'".($item['meter'])."', 
	'".($item['meter_value_PV'])."',
	'".$item["power"]."',
	'".$item['P_PV']."', 
	'".$item['temperature']."')";
	#echo $sql4."\n";

	$result4 = mysqli_query($_link, $sql4);
	
	$r++;
	$prev["timestamp"] = $item["timestamp"];
	$prev["meter"] = $item["meter"];
}
echo $r." Values received";
?>