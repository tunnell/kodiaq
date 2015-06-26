// ****************************************************
// 
// kodiaq Data Acquisition Software
// 
// File    : MasterMongodbConnection.cc
// Author  : Daniel Coderre, LHEP, Universitaet Bern
// Date    : 05.03.2014
// 
// Brief   : Facilitates connection between master and
//           mongodb database in order to update general
//           run information documents.
// 
// ****************************************************

#include "MasterMongodbConnection.hh"

MasterMongodbConnection::MasterMongodbConnection()
{
   fLog=NULL;
   fOptions=NULL;
   fLogDB = NULL;
   fMonitorDB = NULL;
   fRunsDB = NULL;
   mongo::client::initialize();
}

MasterMongodbConnection::MasterMongodbConnection(koLogger *Log)
{
   fLog=Log;
   fOptions=NULL;
   fLogDB = NULL;
   fMonitorDB = NULL;
   fRunsDB = NULL;
   mongo::client::initialize();
}

MasterMongodbConnection::~MasterMongodbConnection()
{
}

int MasterMongodbConnection::SetDBs(string logdb, string monitordb, 
				    string runsdb){

  // Will check success here
  bool logconnected = true, monitorconnected = true, runsconnected = true;

  // If previously defined then close the old connections
  if(fLogDB != NULL)
    delete fLogDB;
  if(fMonitorDB != NULL)
    delete fMonitorDB;
  if(fRunsDB != NULL )
    delete fRunsDB;

  // Connect logdb
  if( logdb != "" ){
    try{
      fLogDB = new mongo::DBClientConnection( true );
      fLogDB->connect( logdb );
    }
    catch(const mongo::DBException &e){
      delete fLogDB;
      fLogDB = NULL;
      if(fLog!=NULL) fLog->Error("Problem connecting to mongo. Caught exception " + 
				 string(e.what()));        
      logconnected = false;
    }
  }
  
  // Connect monitor db
  if( monitordb != "" ){
    try{
      fMonitorDB = new mongo::DBClientConnection( true );
      fMonitorDB->connect( monitordb );
    }
    catch( const mongo::DBException &e ){
      delete fMonitorDB;
      fMonitorDB = NULL;
      if(fLog!=NULL) fLog->Error("Problem connecting to mongo. Caught exception " +
				 string(e.what()));
      monitorconnected = false;
    }
  }
  
  // Connect to runs db
  if( runsdb != ""){
    try{
      fRunsDB = new mongo::DBClientConnection( true );
      fRunsDB->connect( runsdb );
    }
    catch( const mongo::DBException &e ){
      delete fRunsDB;
      fRunsDB = NULL;
      if(fLog!=NULL) fLog->Error("Problem connecting to mongo. Caught exception " +
				 string(e.what()) );
      runsconnected = false;
    }
  }  
  if( logconnected && monitorconnected && runsconnected )
    return 0;
  return -1;
}
 
void MasterMongodbConnection::InsertOnline(string DB, 
					   string collection,
					   mongo::BSONObj bson){
  // Choose proper DB
  mongo::DBClientConnection *thedb = NULL;
  if( DB == "monitor" && fMonitorDB != NULL){
    thedb = fMonitorDB;
  }
  else if( DB=="runs" && fRunsDB != NULL ){
    thedb = fRunsDB;
  }
  else if( DB=="log" && fLogDB != NULL){
    thedb = fLogDB;
  }
  
  // Try for the insertion. If it fails then the DB connection must be bad.
  // to avoid spamming the log files just bring the DB connection down. 
  // we can add a 'reconnect' command to the master to bring it up again if the 
  // problem is solved.
  if(thedb != NULL){
    try{
      thedb->insert(collection,bson);
    }
    catch(const mongo::DBException &e){
      if( fLog!= NULL ){
	fLog->Error("Failed inserting to DB '" + DB + "'. The DB seems to be down or unreachable. Continuing without that DB.");
      }
      if( DB=="monitor"){
	delete fMonitorDB;
	fMonitorDB = NULL;
      }
      else if(DB=="runs"){
	delete fRunsDB;
	fRunsDB = NULL;
      }
      else if(DB=="log"){
	delete fLogDB;
	fLogDB = NULL;
      }	  	      
    }
  }
}

int MasterMongodbConnection::UpdateNoiseDirectory(string run_name){
  if(run_name == "" || fMonitorDB == NULL)
    return -1;

  // See if it exists
  mongo::BSONObjBuilder query;
  query.append("run_name", run_name);
  mongo::BSONObj res = fMonitorDB->findOne("noise.directory" , query.obj() );
  if(res.isEmpty()){    
    mongo::BSONObjBuilder builder;
    builder.append("run_name", run_name);
    builder.append("collection", run_name);
    builder.appendTimeT("date", koLogger::GetCurrentTime() );
    InsertOnline("monitor","noise.directory",builder.obj());
  }
  return 0;
}
int MasterMongodbConnection::InsertRunDoc(string user, string runMode, string name,
					  string comment, string detector, 
					  vector<string> detlist,
					  koOptions *options)
/*
  At run start create a new run document and put it into the online.runs database.
  The OID of this document is saved as a private member so the document can be 
  updated when the run ends. 
  
  Fields important for trigger:
                              
                  runtype :   tells trigger how run should be processed. Hardcoded
                              at the moment but will be part of koOptions once we
                              have different run types
                  starttime:  NOT the UTC time (this is in runstart) but should be
                              the first CAEN digitizer time in the run (usually 0)
		  	     
*/
{

  // First create the collection on the buffer db 
  // so the event builder can find it. Index by time and module.
  mongo::DBClientConnection bufferDB;

  if( options->GetInt("write_mode") == 2 ){ // write to mongo
    try     {
      bufferDB.connect( options->GetString("mongo_address") );
    }
    catch(const mongo::DBException &e)    {
      SendLogMessage( "Problem connecting to mongo buffer. Caught exception " + 
		      string(e.what()), KOMESS_ERROR );
      return -1;
    }
    string collectionName = options->GetString("mongo_database") + "." + 
      options->GetString("mongo_collection");
    bufferDB.createCollection( collectionName );
    bufferDB.createIndex( collectionName,
			  mongo::fromjson( "{ time: -1, module: -1, _id: -1}" ) );
  }
         
  //Create a bson object with the run information
  mongo::BSONObjBuilder builder;
  builder.genOID();

  // event builder sub object
  mongo::BSONObjBuilder trigger_sub; 
  trigger_sub.append( "mode",options->GetString("trigger_mode") );
  trigger_sub.append( "ended", false );
  trigger_sub.append( "status", "waiting_to_be_processed" );
  builder.append( "trigger", trigger_sub.obj() );

  // reader sub object
  mongo::BSONObjBuilder reader_sub;
  reader_sub.append( "compressed", options->GetInt("compression") );
  reader_sub.append( "starttime", 0 );
  reader_sub.append( "data_taking_ended", false );
  reader_sub.append( "options", options->ExportBSON() );  
  mongo::BSONObjBuilder storageSub;
  storageSub.append( "dbname", options->GetString("mongo_database") );
  storageSub.append( "dbcollection", options->GetString("mongo_collection") );
  storageSub.append( "dbaddr", options->GetString("mongo_address") );
  reader_sub.append( "storage_buffer", storageSub.obj() );
  builder.append( "reader", reader_sub.obj() );

  // processor sub object
  mongo::BSONObjBuilder processor_sub;
  processor_sub.append( "mode", "default" ); 
  builder.append( "processor", processor_sub.obj() );

  // top-level fields 
  builder.append("runmode",runMode);
  builder.append("user",user);
  builder.append("name",name);  
  builder.append("detectors", detlist);

  builder.append("shorttype",options->GetString("nickname"));

  //put in start time
  time_t currentTime;
  //struct tm *starttime;
  time(&currentTime);
  //starttime = localtime(&currentTime);
  //long offset = starttime->tm_gmtoff;
  builder.appendTimeT("starttimestamp",currentTime);//+offset);//mktime(starttime));       
  // if comment, add comment sub-object                                              
  if(comment != ""){
    mongo::BSONArrayBuilder comment_arr;
    mongo::BSONObjBuilder comment_sub;    
    comment_sub.append( "text", comment);
    comment_sub.appendTimeT( "date", currentTime);//+offset );
    comment_sub.append( "user", user );
    comment_arr.append( comment_sub.obj() );
    builder.appendArray( "comments", comment_arr.arr() );
  }
   
  //insert into collection
  mongo::BSONObj bObj = builder.obj();
  InsertOnline("runs","online.runs",bObj);

  // store OID so you can update the end time
  mongo::BSONElement OIDElement;
  bObj.getObjectID(OIDElement);
  fLastDocOIDs[detector]=OIDElement.__oid();

  return 0;   
}


int MasterMongodbConnection::UpdateEndTime(string detector)
/*
  When a run is ended we update the run document to indicate that we are 
  finished writing.
  
  Fields updated: 
  
  "endtimestamp": UTC time of end of run
  "data_taking_ended": bool to indicate reader is done with the run
 */
{
  if(fRunsDB == NULL )
    return -1;
  if(!fLastDocOIDs[detector].isSet())  {
    if(fLog!=NULL) fLog->Error("MasterMongodbConnection::UpdateEndTime - Want to stop run but don't have the _id field of the run info doc");
      return -1;
   }
   try  {
  
     // Get the time in GMT
     time_t nowTime;
     //struct tm *currenttime;
     time(&nowTime);
     //currenttime = localtime(&nowTime);
     //currenttime = gmtime(&nowTime);
     //long offset = currenttime->tm_gmtoff;      
     
     mongo::BSONObj res;
     string onlinesubstr = "runs";
     
     mongo::BSONObjBuilder bo;
     bo << "findandmodify" << onlinesubstr.c_str() << 
       "query" << BSON("_id" << fLastDocOIDs[detector]) << 
       "update" << BSON("$set" << BSON("endtimestamp" <<
				       mongo::Date_t(1000*(nowTime))//+offset)) 
				       << "reader.data_taking_ended" << true));     
     assert(fRunsDB->runCommand("runs",bo.obj(),res));
   }
   catch (const mongo::DBException &e) {
     if(fLog!=NULL) fLog->Error("MasterMongodbConnection::UpdateEndTime - Error fetching and updating run info doc with end time stamp.");
     return -1;
   }   
   return 0;
}

void MasterMongodbConnection::SendLogMessage(string message, int priority)
/*
  Log messages are saved into the database. An enum indicates the priority of the message.
  For messages flagged with KOMESS_WARNING or KOMESS_ERROR an alert is created requiring
  a user to mark the alert as solved before starting a new run.
 */
{      

  time_t currentTime;
  //struct tm *starttime;
  time(&currentTime);
  //starttime = localtime(&currentTime);
  //starttime = gmtime(&currentTime);
  //long offset = starttime->tm_gmtoff;

  int ID=-1;
  // For WARNINGS and ERRORS we put an additional entry into the alert DB
  // users then get an immediate alert
  if((priority==KOMESS_WARNING || priority==KOMESS_ERROR) && fMonitorDB!=NULL ){
    mongo::BSONObj obj = fMonitorDB->findOne("online.alerts",
					     mongo::Query().sort("idnum",-1));    
    if(obj.isEmpty())
      ID=0;
    else
      ID = obj.getIntField("idnum")+1;

    mongo::BSONObjBuilder alert;
    alert.genOID();
    alert.append("idnum",ID);
    alert.append("priority",priority);
    alert.appendTimeT("timestamp",currentTime);//+offset);
    alert.append("sender","dispatcher");
    alert.append("message",message);
    alert.append("addressed",false);
    InsertOnline("monitor", "online.alerts",alert.obj());
  }

  stringstream messagestream;
  string savemessage;
  if(priority==KOMESS_WARNING){
    messagestream<<"The dispatcher has issued a warning with ID "<<ID<<" and text: "<<message;
    savemessage=messagestream.str();
  }
  else if(priority == KOMESS_ERROR){
    messagestream<<"The dispatcher has issued an error with ID "<<ID<<" and text: "<<message;
    savemessage=messagestream.str();
  }
  else savemessage=message;

  // Save into normal log
   mongo::BSONObjBuilder b;
   b.genOID();
   b.append("message",message);
   b.append("priority",priority);
   b.appendTimeT("time",currentTime);//+offset);	
   b.append("sender","dispatcher");
   InsertOnline("log", "log.log",b.obj());
  
}

void MasterMongodbConnection::AddRates(koStatusPacket_t *DAQStatus)
/*
  Update the rates in the online db. 
  Idea: combine all rates into one doc to cut down on queries?
  The online.rates collection should be a TTL collection.
*/
{   
  for(unsigned int x = 0; x < DAQStatus->Slaves.size(); x++)  {
      mongo::BSONObjBuilder b;
      time_t currentTime;
      time(&currentTime);
      //struct tm *starttime;
      //starttime = localtime(&currentTime);
      //long offset = starttime->tm_gmtoff;
      //starttime = gmtime(&currentTime);

      b.appendTimeT("createdAt",currentTime);//+offset);
      b.append("node",DAQStatus->Slaves[x].name);
      b.append("bltrate",DAQStatus->Slaves[x].Freq);
      b.append("datarate",DAQStatus->Slaves[x].Rate);
      b.append("runmode",DAQStatus->RunMode);
      b.append("nboards",DAQStatus->Slaves[x].nBoards);
      b.append("timeseconds",(int)currentTime);
      InsertOnline("monitor", "online.daq_rates",b.obj());
   }     
}

void MasterMongodbConnection::UpdateDAQStatus(koStatusPacket_t *DAQStatus,
					      string detector)
/*
  Insert DAQ status doc. The online.daqstatus should be a TTL collection.
 */
{
   mongo::BSONObjBuilder b;
   time_t currentTime;
   time(&currentTime);
   //struct tm *starttime;
   //starttime = localtime(&currentTime);
   //long offset = starttime->tm_gmtoff;

   b.appendTimeT("createdAt",currentTime);//+offset);
   b.append("timeseconds",(int)currentTime);
   b.append("detector", detector);
   b.append("mode",DAQStatus->RunMode);
   if(DAQStatus->DAQState==KODAQ_ARMED)
     b.append("state","Armed");
   else if(DAQStatus->DAQState==KODAQ_RUNNING)
     b.append("state","Running");
   else if(DAQStatus->DAQState==KODAQ_IDLE)
     b.append("state","Idle");
   else if(DAQStatus->DAQState==KODAQ_ERROR)
     b.append("state","Error");
   else
     b.append("state","Undefined");
   b.append("network",DAQStatus->NetworkUp);
   b.append("currentRun",DAQStatus->RunInfo.RunNumber);
   b.append("startedBy",DAQStatus->RunInfo.StartedBy);
   
   //start time
   //   struct tm timeobj;
   //strptime(DAQStatus->RunInfo.StartDate.c_str(), "%Y.%m.%d [%H:%M:%S]", &timeobj);
   string datestring = DAQStatus->RunInfo.StartDate;
   if(datestring.size()!=0){
     datestring.pop_back();
     datestring+="+01:00";
   }
   b.append("startTime",datestring);
   b.append("numSlaves",(int)DAQStatus->Slaves.size());
   InsertOnline("monitor", "online.daq_status",b.obj());
}

int MasterMongodbConnection::CheckForCommand(string &command, string &user, 
					     string &comment, string &detector,
					     koOptions &options)
/*
  DAQ commands can be written to the online.daqcommand db. These usually come from the web
  interface but they can actually come from anywhere. The only commands recognized are 
  "Start" and "Stop". The run mode (for "Start") and comments (for both) are also pulled
  from the doc.
 */
{
  if(fMonitorDB == NULL )
    return -1;
  if(fMonitorDB->count("online.daq_commands") ==0)
    return -1;
   auto_ptr<mongo::DBClientCursor> cursor = fMonitorDB->query("online.daqcommands",mongo::BSONObj());
   mongo::BSONObj b;
   if(cursor->more())
     b = cursor->next();
   else
     return -1;
   command=b.getStringField("command");
   string mode=b.getStringField("mode");
   comment = b.getStringField("comment");
   detector = b.getStringField("detector");
   user=b.getStringField("name");
   fMonitorDB->remove("online.daq_commands", 
		   MONGO_QUERY("command"<<"Start"<<"detector"<<detector));
   fMonitorDB->remove("online.daq_commands",
		   MONGO_QUERY("command"<<"Stop"<<"detector"<<detector)); 
   if(command=="Start")
     PullRunMode(mode,options);
   return 0;
}

void MasterMongodbConnection::SendRunStartReply(int response, string message, 
						string mode, string comment)
/*
  When starting a run a plausibility check is performed. The result of this 
  check is sent using this function. The web interface will wait for an 
  entry in this DB and notify the user if something goes wrong.
 */
{
  mongo::BSONObjBuilder reply;
  reply.append("message",message);
  reply.append("replyenum",response);
  reply.append("mode",mode);
  reply.append("comment",comment);
  InsertOnline("monitor", "online.dispatcherreply",reply.obj());
}

int MasterMongodbConnection::PullRunMode(string name, koOptions &options)
/*
  Converts a mongodb run mode document to a koOptions object. Would be easier
  in python since we could just save the dict. C++ requires we loop through 
  each attribute and set it.
 */
{
  if(fMonitorDB == NULL) return -1;
   if(fMonitorDB->count("online.run_modes") ==0)
     return -1;

   //Find doc corresponding to this run mode
   mongo::BSONObjBuilder query; 
   query.append( "name" , name ); 
   //cout<<"Looking for run mode "<<name<<endl;
   mongo::BSONObj res = fMonitorDB->findOne("online.run_modes" , query.obj() ); 
   if(res.nFields()==0) 
     return -1; //empty object
   options.SetBSON(res);
   return 0;
   /*
   //Set Options From Mongo
   //cout<<"Retrieved mongo run mode "<<name<<endl;
   options.name=(res.getStringField("name"));
   options.nickname=(res.getStringField("nickname"));
   options.creator=(res.getStringField("creator"));
   options.creation_date=(res.getStringField("creation_date"));
   options.write_mode=(res.getIntField("write_mode"));
   options.trigger_mode = res.getStringField("trigger_mode");
   options.data_processor_mode = res.getStringField("data_processor_mode");
   options.noise_spectra_enable = res.getIntField("noise_spectra_enable");
   options.noise_spectra_length = res.getIntField("noise_spectra_length");
   options.noise_spectra_mongo_addr = res.getStringField("noise_spectra_mongo_addr");
   options.noise_spectra_mongo_coll = res.getStringField("noise_spectra_mongo_coll");
   options.run_prefix = res.getStringField("run_prefix");
   options.noise_spectra = res.getIntField("noise_spectra");
   options.baseline_mode=(res.getIntField("baseline_mode"));
   options.run_start=(res.getIntField("run_start"));
   options.run_start_module=(res.getIntField("run_start_module"));
   options.pulser_freq=(res.getIntField("pulser_freq"));
   options.blt_size=(res.getIntField("blt_size"));
   options.compression=(res.getIntField("compression"));
   options.processing_mode=(res.getIntField("processing_mode"));
   options.processing_num_threads=(res.getIntField("processing_num_threads"));
   options.processing_readout_threshold=(res.getIntField("processing_readout_threshold"));
   options.occurrence_integral = (res.getBoolField("occurrence_integral"));
   options.mongo_address=(res.getStringField("mongo_address"));
   options.mongo_database=(res.getStringField("mongo_database"));
   options.mongo_collection=(res.getStringField("mongo_collection"));
   options.mongo_output_format = res.getStringField("mongo_output_format");
   // Check for dynamic run name character
   if( options.mongo_collection[options.mongo_collection.size()-1] == '*' ) {
     options.mongo_collection = options.mongo_collection.
       substr( 0, options.mongo_collection.size() - 1 );
     options.dynamic_run_names = true;
   }

   options.mongo_write_concern=(res.getIntField("mongo_write_concern"));
   options.mongo_min_insert_size=(res.getIntField("mongo_min_insert_size"));
   options.file_path=(res.getIntField("file_path"));
   options.file_events_per_file=(res.getIntField("file_events_per_file"));
   options.led_trigger = (res.getIntField("led_trigger"));
   options.muon_veto = (res.getIntField("muon_veto"));

   // Do we have DDC-10 options? Then fill them
   mongo::BSONObj ddcdict = res.getObjectField("ddc10_options");
   set<string> fieldNames;
   ddcdict.getFieldNames(fieldNames);
   
   stringstream ddcstream;
   set<string>::iterator it;
   for (it = fieldNames.begin(); it != fieldNames.end(); it++){
     if(*it == "ddc10_ip_address")
       ddcstream<<*it<<" "<<ddcdict.getField(*it).String()<<endl;
     else if(*it == "ddc10_enable"){
       bool enableddc = ddcdict.getField(*it).Bool();
       ddcstream<<*it<<" ";
       if(enableddc) ddcstream<<"1"<<endl;
       else ddcstream<<"0"<<endl;
     }
     else
       ddcstream<<*it<<" "<<ddcdict.getField(*it).Int()<<endl;
   }
   options.SetDDCStream(ddcstream.str());

   // registers, boards, and crates
   
   //registers
   vector<mongo::BSONElement> regs = res.getField("registers").Array();
   for(unsigned int x=0;x<regs.size();x++){
     vme_option_t reg;
     reg.address=koHelper::StringToHex(regs[x]["reg"].String());
     reg.value=koHelper::StringToHex(regs[x]["val"].String());
     reg.board=(regs[x]["board"].Int());
     reg.node='x';
     options.AddVMEOption(reg);
   }
   //links
   vector<mongo::BSONElement> links = res.getField("links").Array();
   for(unsigned int x=0;x<links.size();x++){
     link_definition_t link;
     link.type="V2718"; //only one supported a.t.m... CAEN thing.
     link.id = links[x]["link"].Int();
     link.crate = links[x]["crate"].Int();
     link.node = koHelper::IntToString(links[x]["reader"].Int())[0];
     options.AddLink(link);
   }  
   //boards
   vector<mongo::BSONElement> boards = res.getField("boards").Array();
   for(unsigned int x=0;x<boards.size();x++){
     board_definition_t board;
     //cout<<"Found board "<<boards[x]["serial"].Int()<<endl;
     board.type = boards[x]["boardtype"].String();
     board.vme_address = koHelper::StringToHex(boards[x]["vme"].String());
     board.id = boards[x]["serial"].Int();
     board.crate = boards[x]["crate"].Int();
     board.link = boards[x]["link"].Int();
     board.node = koHelper::IntToString(boards[x]["reader"].Int())[0];
     options.AddBoard(board);
   }
   return 0;
   */
}
