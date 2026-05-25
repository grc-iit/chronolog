# Cluster setup

* One dedicated node for ChronoVisor
* 1, 2, 4 dedicated nodes for ChronoKeeper
* 1, 2, 4 dedicated nodes for ChronoGrapher
* 1, 2, 4 dedicated node for ChronoPlayer

# Communication protocol configurations

* ofi+sockets
* ofi+verbs (only supported in KeeperRecordingService and KeeperGrapherDrainService)

# chronolog-test-performance configurations

* Connection/disconnection throughput (barrier=ON, #client=1x1,10x1,10x2,20x1,10x4,20x2,40x1,20x4,40x2,40x4)
* Acquisition/release throughput (barrier=ON, #story\_per\_proc=#client, #client=1x1,10x1,10x2,20x1,10x4,20x2,40x1,20x4,40x2,40x4)
* Recording bandwidth (barrier=ON, #story\_per\_proc=#client, #client=1x1,10x1,10x2,20x1,10x4,20x2,40x1,20x4,40x2,40x4)
* Replay bandwidth (barrier=ON, read=ON, #story\_per\_proc=#client, #client=1x1,10x1,10x2,20x1,10x4,20x2,40x1,20x4,40x2,40x4)

# Repetition

* \#reps=3
