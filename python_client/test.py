
//make install

// currently CMAKE_INSTALL_LIBDIR=/home/ENV{USERNAME}/chronolog/lib

// create symlink for  py_chronolog_client.[linux_distibution_version].so 
// ln -s ${CMAKE_INSTALL_LIBDIR}/py_chronolog_client.so ${CMAKE_INSTALL_LIBDIR}/py_chronolog_client.[linux_distribution_version].so

//update LD_LIBRARY_PATH
// export LD_LIBRARY_PATH=${CMAKE_INSTALL_LIBDIR}:${LD_LIBRARY_PATH}
//update PYTHONPATH
// export PYTHONPATH=${CMAKE_INSTALL_LIBDIR}:${PYTHONPATH}


//using spack installed python3 version

>>> import py_chronolog_client

>>> attrs=dict();
>>> clientConf = py_chronolog_client.ClientPortalServiceConf("ofi+sockets","127.0.0.1",5555,55);

>>> client = py_chronolog_client.Client(clientConf);

>>> client.CreateChronicle("py_chronicle", attrs, 1);
-15
>>> client.DestroyChronicle("py_chronicle");
-15
>>> return_pair = client.AcquireStory("py_chronicle", "my_story", attrs, 1);
>>> print(return_pair)
(-15, None)
>>> 

