<?php
#ini_set('display_errors', 1);
#ini_set('display_startup_errors', 1);
#error_reporting(E_ALL);

include("valid_clients.php");

$id = $_GET['ID'] ?? '';
$token = $_GET['token'] ?? '';

// check if client is valid
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

// read binary data from the request body
$rawData = file_get_contents('php://input');

// calculate entry size
$entrySize = 4 + 4 + 4; // timestamp (4 Bytes) + meter (4 Bytes) + temperature (4 Bytes)
if($PV_included) $entrySize = 4 + 4 + 4 + 4; // timestamp (4 Bytes) + meter (4 Bytes) + temperature (4 Bytes) + solar (4)

$dataCount = strlen($rawData) / $entrySize;

if ($dataCount != floor($dataCount)) {
    http_response_code(400);
    echo "Invalid data length.";
    exit;
}


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

// sort array by timestamp
usort($data["values"], function ($a, $b) {
    return $a["timestamp"] <=> $b["timestamp"];
});

http_response_code(200);
echo "Data received and processed successfully.";


if($value_count > 5) 
{
	// create log filename with date and ID
	$filename = date("y-m-d-H-i-s") . "-".$data["ID"].".txt";

	$file = fopen("log/".$filename, "w");
	if ($file === false) {
		die("Fehler beim Öffnen der Datei");
	}

	
	foreach ($data["values"] as $entry) {
		
		$timestamp = $entry["timestamp"];
		$meter = $entry["meter"];
		$temperature = $entry["temperature"];
        $meter_solar = $entry["meter_solar"];

		
		$line = "$timestamp;$meter;$temperature;$meter_solar\n";

		
		fwrite($file, $line);
	}

	
	fclose($file);
}


$prev["meter"] = 0;


include("../config.php");

$sql = "SELECT `meter_value`, `timestamp_client`, `id` FROM `sml_v1` WHERE `id` = '".$data["ID"]."' order by `timestamp_client` DESC LIMIT 1";
$result = mysqli_query($_link, $sql);
while($row = mysqli_fetch_array($result))
{
	$prev["meter"] = $row['meter_value'];
	$prev["timestamp"] = $row['timestamp_client'];
}

$r = 0;
$current_time = time();
foreach ($data["values"] as $item) {
	

	echo $item["timestamp"]." ".$item["meter"]." ".$item["meter_solar"]."\n";
	
	$item["power"] = (($item["meter"] - $prev["meter"])/10)/(($item["timestamp"] - $prev["timestamp"])/3600);
    
	echo $item["timestamp"]." ".$item["meter"]." ".$item["power"]."\n";

	if($item["power"] == 0 || $item["power"] > 40000) continue;
	if($prev["meter"] > $item['meter']) 
	{
		continue; // dismiss this values if lower than the previous one.
	}
	$prev["meter"] = $item["meter"];

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
	'".$item["timestamp"]."', 
	'".($item['meter'])."', 
	'".($item["meter_solar"])."',
	'".$item['temperature']."')";


	$result4 = mysqli_query($_link, $sql4);
	
	$r++;
}
echo $r." Values received";
?>