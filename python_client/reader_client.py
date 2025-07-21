
# ChronoLog project  uses this default installation directory
# CMAKE_INSTALL_LIBDIR=/home/ENV{USERNAME}/chronolog/lib
#
# "make install"  will build chronolog_client.so and py_chronolog_client.[python-version-linux-version].so
# and place them under ${CMAKE_INSTALL_LIBDIR}
# 

## create symlink for  py_chronolog_client.[python-version-linux-version].so 
# so that you can import the binding module as "py_chronolog_client" in Python code
#
## ln -s ${CMAKE_INSTALL_LIBDIR}/py_chronolog_client.[python-version-linux-version].so {CMAKE_INSTALL_LIBDIR}/py_chronolog_client.so

## update LD_LIBRARY_PATH
## export LD_LIBRARY_PATH=${CMAKE_INSTALL_LIBDIR}:${LD_LIBRARY_PATH}
## update PYTHONPATH
## export PYTHONPATH=${CMAKE_INSTALL_LIBDIR}:${PYTHONPATH}


## use ChronoLog spack installed python3 version to avoid version mismatch
#
# for example to run the basic test below 
#
# ${CHRONOLOG_SOURCE_DIR}/.spack-env/view/bin/python3 ${CHRONOLOG_SOURCE_DIR}/python_client/test.py
#
# 
#############################

def reader_client():

    import py_chronolog_client

    print("Basic test for py_chronolog_client reader ")
    
    #create ClientPortalServiceConf instance with the connection credentials to ChronoVisor ClientPortalService

    clientConf = py_chronolog_client.ClientPortalServiceConf("ofi+sockets","127.0.0.1",5555,55);
    clientQueryConf = py_chronolog_client.ClientQueryServiceConf("ofi+sockets","127.0.0.1",5557,57);

    # instantiate ChronoLog Client object
    reader_client = py_chronolog_client.Client(clientConf, clientQueryConf);

    return_code = reader_client.Connect()
    print( "\n client.Connect() call returns:", return_code)

    attrs=dict();
    return_tuple = reader_client.AcquireStory("py_chronicle", "my_story", attrs, 1);
    print( "\n client.AcquireStory() returned:" , return_tuple)

    event_series = py_chronolog_client.EventList()   
    return_code = reader_client.ReplayStory("py_chronicle","my_story", 1, 2000000000000000000, event_series);

    print( "\n client.ReplayStory() call returns:", return_code)
    print( "\n client.ReplayStory() call returns event_series : ", event_series, "with ", len(event_series)," events")

    if len(event_series) >0 :
        for event in event_series:
            print("event:", event.time(),event.client_id(),event.index(),event.log_record())
    
    # release acquired Story
    # returns 0 on success and error_code othewise
    return_code = reader_client.ReleaseStory("py_chronicle","my_story");
    print("\n client.ReleaseStory() returned:", return_code);

    # Disconenct the client from ChronoLog system
    # returns 0 on success and error_code otherwise
    return_code = reader_client.Disconnect()
    print("\n client.Disconnect() returned:", return_code);


if __name__=="__main__":

    reader_client()
