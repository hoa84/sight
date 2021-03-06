// Copyright (c) 2010, Lawrence Livermore National Security, LLC. Produced at
// the Lawrence Livermore National Laboratory. LLNL-CODE-443211. All Rights
// reserved. See file COPYRIGHT for details.
//
// This file is part of the MFEM library. For more information and source code
// availability see http://mfem.googlecode.com.
//
// MFEM is free software; you can redistribute it and/or modify it under the
// terms of the GNU Lesser General Public License (as published by the Free
// Software Foundation) version 2.1 dated February 1999.

#include "isockstream.hpp"
#include <iostream>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>


isockstream::isockstream(int port)
{
   portnum = port;

   if ( (portID = establish()) < 0)
      cout << "Server couldn't be established on port "
           << portnum << endl;
   Buf = NULL;
}

int isockstream::establish()
{
   // char myname[129];
   char   myname[] = "localhost";
   int    port;
   struct sockaddr_in sa;
   struct hostent *hp;

   memset(&sa, 0, sizeof(struct sockaddr_in));
   // gethostname(myname, 128);
   hp= gethostbyname(myname);

   if (hp == NULL)
   {
      cerr << "isockstream::establish(): gethostbyname() failed!\n"
           << "isockstream::establish(): gethostname() returned: '"
           << myname << "'" << endl;
      error = 1;
      return(-1);
   }

   sa.sin_family= hp->h_addrtype;
   sa.sin_port= htons(portnum);

   if ((port = socket(AF_INET, SOCK_STREAM, 0)) < 0)
   {
      cerr << "isockstream::establish(): socket() failed!" << endl;
      error = 2;
      return(-1);
   }

   int on=1;
   setsockopt(port, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

   if (bind(port,(const sockaddr*)&sa,sizeof(struct sockaddr_in)) < 0)
   {
      cerr << "isockstream::establish(): bind() failed!" << endl;
      close(port);
      error = 3;
      return(-1);
   }

   listen(port, 4);
   error = 0;
   return(port);
}

int isockstream::read_data(int s, char *buf, int n){
   int bcount;                      // counts bytes read
   int br;                          // bytes read this pass

   bcount= 0;
   br= 0;
   while (bcount < n) {             // loop until full buffer
      if ((br= read(s,buf,n-bcount)) > 0) {
         bcount += br;                // increment byte counter
         buf += br;                   // move buffer ptr for next read
      }
      else if (br < 0)               // signal an error to the caller
      {
         error = 4;
         return(-1);
      }
   }
   return(bcount);
}

void isockstream::receive(istringstream **in)
{
   int size;
   char length[32];

   if ((*in) != NULL)
      delete (*in), *in = NULL;

   if (portID == -1)
      return;

   if ((socketID = accept(portID, NULL, NULL)) < 0)
   {
      cout << "Server failed to accept connection." << endl;
      error = 5;
      return;
   }

   if (read(socketID, length, 32) < 0)
   {
      error = 6;
      return;
   }
   size = atoi(length);

   if (Buf != NULL)
      delete [] Buf;
   Buf = new char[size+1];
   if (size != read_data(socketID, Buf, size))
      cout << "Not all the data has been read" << endl;
#ifdef DEBUG
   else
      cout << "Reading " << size << " bytes is successful" << endl;
#endif
   Buf[size] = '\0';

   close(socketID);
   (*in) = new istringstream(Buf);
}

isockstream::~isockstream()
{
   if (Buf != NULL)
      delete [] Buf;
   if (portID != -1)
      close(portID);
}
