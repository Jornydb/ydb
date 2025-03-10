$identity = ($x)-> { return $x };

$idDate32 = Callable(Callable<(date32)->date32>, $identity);
$idDatetime64 = Callable(Callable<(datetime64)->datetime64>, $identity);
$idTimestamp64 = Callable(Callable<(timestamp64)->timestamp64>, $identity);
$idInterval64 = Callable(Callable<(interval64)->interval64>, $identity);

$valDate = unwrap(cast(1 as date));
$valDate32 = unwrap(cast(-1 as date32));
$valDatetime = unwrap(cast(86400 as datetime));
$valDatetime64 = unwrap(cast(-86400 as datetime64));
$valTimestamp = unwrap(cast(86400l*1000000 as timestamp));
$valTimestamp64 = unwrap(cast(86400l*1000000 as timestamp64));
$valInterval = unwrap(cast(1 as interval));
$valInterval64 = unwrap(cast(-1 as interval64));

select 1, $idDate32($valDate), $idDate32($valDate32)
, 2, $idDatetime64($valDate), $idDatetime64($valDate32)
, $idDatetime64($valDatetime), $idDatetime64($valDatetime64)
, 3, $idTimestamp64($valDate), $idTimestamp64($valDate32)
, $idTimestamp64($valDatetime), $idTimestamp64($valDatetime64)
, $idTimestamp64($valTimestamp), $idTimestamp64($valTimestamp64)
, 4, $idInterval64($valInterval), $idInterval64($valInterval64);
