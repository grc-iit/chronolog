
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

def main():

    print("Basic test for py_chronolog_client")

    # NOTE: if you having issues importing py_chronolog_client
    # see post installation instructions at the top of this file
 
    import py_chronolog_client

    #create ClientPortalServiceConf instance with the connection credentials to ChronoVisor ClientPortalService

    clientConf = py_chronolog_client.ClientPortalServiceConf("ofi+sockets","127.0.0.1",5555,55);

    # instantiate ChronoLog Client object
    client = py_chronolog_client.Client(clientConf);

    #try to acquire the story 
    # this should fail as the connection to the ChronoVisor hasn't been established yet
    attrs=dict();
    return_tuple = client.AcquireStory("py_chronicle", "my_story", attrs, 1);
    print("\n Attempt to acquire story without ChronoVisor connection returns : ", return_tuple)
    # (-15, None)


    # Connect to the ChronoVisor for client authentication
    # returns 0 on success and error_cde otherwise
    return_code = client.Connect()
    print( "\n client.Connect() call returns:", return_code)

    #create chronicle
    return_code = client.CreateChronicle("py_chronicle", attrs, 1);
    print("\n clientCreateCronicle() returned", return_code);

    # acquire story that is part of "py_chronicle"
    # returns a tuple [0, StoryHandle] on success
    # and [error_code,None] otherwise
    return_tuple = client.AcquireStory("py_chronicle", "my_story", attrs, 1);
    
    print( "\n client.AcquireStory() returned:" , return_tuple)

    if( return_tuple[0] == 0):
    
        print( "\n Aquired Story = my_story within chronicle = py_chronicle");

        print("\n logging 4 events for my_story using the StoryHandle");
        return_tuple[1].log_event("py_event");
        return_tuple[1].log_event("py_event.2");
        return_tuple[1].log_event("py_event.3");
        return_tuple[1].log_event("py_event.4");
    

    # release acquired Story
    # returns 0 on success and error_code othewise
    return_code = client.ReleaseStory("py_chronicle","my_story");
    print("\n client.ReleaseStory() returned:", return_code);
    
    
        
    # Disconenct the client from ChronoLog system
    # returns 0 on success and error_code otherwise
    return_code = client.Disconnect()
    print("\n client.Disconnect() returned:", return_code);

if __name__=="__main__":
    main()
