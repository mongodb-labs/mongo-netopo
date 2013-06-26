<?php

$host = $_POST["hostname"];

echo "you pinged : " . $host . "<br/><br/>";

try{

$conn = new Mongo( $host );

/*
$config = $conn->config;
$shards = $config->shards;
$cursor = $shards->find();
echo "shards in this cluster : <br/>";


foreach ($cursor as $document) {
    echo "id : " . $document["_id"] . "  ";
    echo " , host : " . $document["host"] . "<br/>";
}

echo "count : " . $cursor->count();

echo "<pre>";
while( $cursor->hasNext()){
	$document = $cursor->getNext();
	print_r($document);
		echo PHP_EOL;
}
echo "</pre>";
*/

$admin = $conn->admin;

echo "connected to : " . $admin . "<br/><br/>";

$result = $admin->command(array("ping"=>1, "hosts"=>array("localhost:30999", "localhost:27017")));
$result_r = var_export($result);

echo "<br/><br/>";

echo "success " . ($result["ok"] ? "yes" : "no") . "<br/><br/>" ;

/*foreach ($result as $doc){
	echo print_r($doc) . "<br/>";
}

echo "<br/><br/>";

echo $result_r;

//echo print_r($result);

*/







  // disconnect from server
  $conn->close();
} catch (MongoConnectionException $e) {
  die('Error connecting to MongoDB server');
} catch (MongoException $e) {
  die('Error: ' . $e->getMessage());
}

?>