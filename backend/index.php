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

$PV_included = false;;
if(isset($_GET['PV_included']) && $_GET['PV_included'] == "true") 
{
	$PV_included = true;
} 

// Binärdaten auslesen
$rawData = file_get_contents('php://input');

// Anzahl der Einträge berechnen
$entrySize = 4 + 4 + 4; // timestamp (4 Bytes) + meter (4 Bytes) + temperature (4 Bytes)
if($PV_included) $entrySize = 4 + 4 + 4 + 4; // timestamp (4 Bytes) + meter (4 Bytes) + temperature (4 Bytes) + solar (4)

$dataCount = strlen($rawData) / $entrySize;

if ($dataCount != floor($dataCount)) {
    http_response_code(400);
    echo "Invalid data length.";
    exit;
}

// Binärdaten in Einträge zerlegen
$value_count = 0;
$meter_solar = 0;
for ($i = 0; $i < $dataCount; $i++) {
    $offset = $i * $entrySize;
    $timestamp = unpack("L", substr($rawData, $offset, 4))[1];
	$meter = unpack("L", substr($rawData, $offset + 4, 4))[1];
    $temperature = unpack("L", substr($rawData, $offset + 8, 4))[1];
	
	if($PV_included) $meter_solar = unpack("L", substr($rawData, $offset + 12, 4))[1];
	
	if($meter == 0) continue;
	#if($temperature > 200) $temperature = -3;
	$value_count++;
    $data["values"][] = [
        "timestamp" => $timestamp,
        "meter" => $meter,
        "temperature" => $temperature,
		"meter_solar" => $meter_solar,		
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
	$filename = date("y-m-d-H-i-s") . "-".$data["ID"].".txt";

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
		$meter_solar = $entry["meter_solar"];

		// Erstelle die Zeile im gewünschten Format
		$line = "$timestamp;$meter;$temperature;$meter_solar\n";

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


$r = 0;
$current_time = time();
foreach ($data["values"] as $item) {
	
	if(($item["timestamp"] - $prev["timestamp"]) == 0) continue;
	if($prev["meter"] > $item['meter'])
	{
		$prev["meter"] = $item["meter"];
		break; // dismiss all values if one was lower than the previous one.
	}
	$item["power"] = (($item["meter"] - $prev["meter"])/10)/(($item["timestamp"] - $prev["timestamp"])/3600);
    $item["power"] = round($item["power"], 0);
	echo $item["timestamp"]." ".$item["meter"]." ".$item["power"]."\n";
	if($item["power"] == 0 || $item["power"] > 300000) continue;
	if($item['meter'] < 0 ) $item['meter'] = 0;
	if(!isset($item['temperature'])) $item['temperature'] = 0;
	$item['temperature'] = $item['temperature']/100;

	if(!isset($item['meter_solar'])) $item['meter_solar'] = 0;
	
	$sql4 = "INSERT INTO `sml_v1` (
	`i`, 
	`id`, 
	`timestamp_server`, 
	`timestamp_server2`,
	`timestamp_client`, 
	`meter_value`, 
	`meter_value_PV`, 
	`temperature`) 
	VALUES (
	NULL, 
	'".$data['ID']."', 
	'".date('Y-m-d H:i:s', $current_time)."', 
	".$current_time.",
	'".date('Y-m-d H:i:s', $current_time)."', 
	".$current_time.",
	'".$item["timestamp"]."', 
	'".($item['meter'])."', 
	'".($item["meter_solar"])."',
	'".$item['temperature']."')";
	#echo $sql4."\n";

	$result4 = mysqli_query($_link, $sql4);
	
	$r++;
	$prev["timestamp"] = $item["timestamp"];
	$prev["meter"] = $item["meter"];
}
echo $r." Values received";
?>