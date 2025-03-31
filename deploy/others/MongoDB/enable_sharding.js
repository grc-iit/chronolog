use integration
sh.enableSharding('integration')
db.createCollection('symbios')
var shardKey = { "_id": "hashed" }
sh.shardCollection('integration.symbios', shardKey)
db.symbios.getShardVersion()
db.symbios.getShardDistribution()
