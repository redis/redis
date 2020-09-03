/*
 * Copyright (c) <2019> whoever, I don't care
 */
provider redis {
   /**
    * Fired before a command is called
    * @param id the command
    */
   probe call__start(int cmd_id);

   /**
    * Fired after a command is called
    * @param id the command
    */
   probe call__end(int cmd_id);

};

#pragma D attributes Unstable/Unstable/Common provider redis provider
#pragma D attributes Private/Private/Common provider redis module
#pragma D attributes Private/Private/Common provider redis function
#pragma D attributes Unstable/Unstable/Common provider redis name
#pragma D attributes Unstable/Unstable/Common provider redis args
