
# make install

# currently CMAKE_INSTALL_LIBDIR=/home/ENV{USERNAME}/chronolog/lib

## create symlink for  py_chronolog_client.[linux_distibution_version].so 
## ln -s ${CMAKE_INSTALL_LIBDIR}/py_chronolog_client.so ${CMAKE_INSTALL_LIBDIR}/py_chronolog_client.[linux_distribution_version].so

## update LD_LIBRARY_PATH
## export LD_LIBRARY_PATH=${CMAKE_INSTALL_LIBDIR}:${LD_LIBRARY_PATH}
## update PYTHONPATH
## export PYTHONPATH=${CMAKE_INSTALL_LIBDIR}:${PYTHONPATH}


## using spack installed python3 version

#############################

import py_chronolog_client

>>>attrs=dict();

>>>clientConf = py_chronolog_client.ClientPortalServiceConf("ofi+sockets","127.0.0.1",5555,55);

>>>client = py_chronolog_client.Client(clientConf);

>>>return_pair = client.AcquireStory("py_chronicle", "my_story", attrs, 1);
>>>print(return_pair)
(-15, None)
>>> 

>>> client.Connect()
0
>>> client.CreateChronicle("py_chronicle", attrs, 1);
0
>>> return_pair = client.AcquireStory("py_chronicle", "my_story", attrs, 1);
>>> print(return_pair)
(0, <py_chronolog_client.StoryHandle object at 0xffffa135e5b0>)
>>> print(return_pair[1])
<py_chronolog_client.StoryHandle object at 0xffffa135e5b0>

>>> return_pair[1].log_event("py_event")
1
>>> return_pair[1].log_event("py_event.2")
1
>>> return_pair[1].log_event("py_event.3")
1
>>> return_pair[1].log_event("py_event.4")
1
>>> client.ReleaseStory("py_chronicle","py_story");
-3
>>> client.ReleaseStory("py_chronicle","my_story");
0
>>> client.Disconnect()
0

