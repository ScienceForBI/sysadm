// ===============================
//  PC-BSD REST/JSON API Server
// Available under the 3-clause BSD License
// Written by: Ken Moore <ken@pcbsd.org> July 2015
// =================================
#include "WebSocket.h"

#include <QtConcurrent>
#include <unistd.h>

#define DEBUG 0
#define IDLETIMEOUTMINS 30

WebSocket::WebSocket(QWebSocket *sock, QString ID, AuthorizationManager *auth){
  SockID = ID;
  SockAuthToken.clear(); //nothing set initially
  SOCKET = sock;
  TSOCKET = 0;
  AUTHSYSTEM = auth;
  SockPeerIP = SOCKET->peerAddress().toString();
  LogManager::log(LogManager::HOST,"New Connection: "+SockPeerIP);
  idletimer = new QTimer(this);
    idletimer->setInterval(IDLETIMEOUTMINS*60000); //connection timout for idle sockets
    idletimer->setSingleShot(true);
  connect(idletimer, SIGNAL(timeout()), this, SLOT(checkIdle()) );
  connect(SOCKET, SIGNAL(textMessageReceived(const QString&)), this, SLOT(EvaluateMessage(const QString&)) );
  connect(SOCKET, SIGNAL(binaryMessageReceived(const QByteArray&)), this, SLOT(EvaluateMessage(const QByteArray&)) );
  connect(SOCKET, SIGNAL(aboutToClose()), this, SLOT(SocketClosing()) );
  idletimer->start();
}

WebSocket::WebSocket(QSslSocket *sock, QString ID, AuthorizationManager *auth){
  SockID = ID;
  SockAuthToken.clear(); //nothing set initially
  TSOCKET = sock;
  SOCKET = 0;
  SockPeerIP = TSOCKET->peerAddress().toString();
  LogManager::log(LogManager::HOST,"New Connection: "+SockPeerIP);
  AUTHSYSTEM = auth;
  idletimer = new QTimer(this);
    idletimer->setInterval(IDLETIMEOUTMINS*60000); //connection timout for idle sockets
    idletimer->setSingleShot(true);
  connect(idletimer, SIGNAL(timeout()), this, SLOT(checkIdle()) );
  connect(TSOCKET, SIGNAL(readyRead()), this, SLOT(EvaluateTcpMessage()) );
  connect(TSOCKET, SIGNAL(aboutToClose()), this, SLOT(SocketClosing()) );
  connect(TSOCKET, SIGNAL(encrypted()), this, SLOT(nowEncrypted()) );
  connect(TSOCKET, SIGNAL(peerVerifyError(const QSslError &)), this, SLOT(peerError(const QSslError &)) );
  connect(TSOCKET, SIGNAL(sslErrors(const QList<QSslError> &)), this, SLOT(SslError(const QList<QSslError> &)) );
  //qDebug() << " - Starting Server Encryption Handshake";
   TSOCKET->startServerEncryption();
  //qDebug() << " - Socket Encrypted:" << TSOCKET->isEncrypted();
  idletimer->start();
}

WebSocket::~WebSocket(){
  //qDebug() << "SOCKET Destroyed";
  if(SOCKET!=0){
    SOCKET->close();
    delete SOCKET;
  }
  if(TSOCKET!=0){
    TSOCKET->close();
    delete TSOCKET;
  }
}


QString WebSocket::ID(){
  return SockID;
}

//=======================
//             PRIVATE
//=======================
void WebSocket::sendReply(QString msg){
  //qDebug() << "Sending Socket Reply:" << msg;
 if(SOCKET!=0 && SOCKET->isValid()){ SOCKET->sendTextMessage(msg); } //Websocket connection
 else if(TSOCKET!=0 && TSOCKET->isValid()){ 
    //TCP Socket connection
    TSOCKET->write(msg.toUtf8().data()); 
    TSOCKET->disconnectFromHost(); //TCP/REST connections are 1 connection per message.
 }
}

void WebSocket::EvaluateREST(QString msg){
  //Parse the message into it's elements and proceed to the main data evaluation
  RestInputStruct IN(msg);
  //NOTE: All the REST functionality is disabled for the moment, until we decide to turn it on again at a later time (just need websockets right now - not full REST)	

  if(DEBUG){
    qDebug() << "New REST Message:";
    qDebug() << "  VERB:" << IN.VERB << "URI:" << IN.URI;
    qDebug() << "  HEADERS:" << IN.Header;
    qDebug() << "  BODY:" << IN.Body;
    //qDebug() << " Auth:" << IN.auth;
    qDebug() << "JSON Values:";
    qDebug() << " - Name:" << IN.name;
    qDebug() << " - Namespace:" << IN.namesp;
    qDebug() << " - ID:" << IN.id;
    qDebug() << " - Has Args:" << !IN.args.isNull();
  }
  //Now check for the REST-specific verbs/actions
  if(IN.VERB == "OPTIONS" || IN.VERB == "HEAD"){
    RestOutputStruct out;	  
      out.in_struct = IN;
      out.CODE = RestOutputStruct::OK;
      if(IN.VERB=="HEAD"){
	
      }else{ //OPTIONS
	out.Header << "Allow: HEAD, GET";
	out.Header << "Hosts: /syscache";	      
      }
      out.Header << "Accept: text/json";
      out.Header << "Content-Type: text/json; charset=utf-8";
    this->sendReply(out.assembleMessage());
  }else{
    //EvaluateRequest(IN);
    if(IN.name.startsWith("auth") ){
      //Keep auth system requests in order
      EvaluateRequest(IN);
    }else{
      QtConcurrent::run(this, &WebSocket::EvaluateRequest, IN);
    }
  }
}

void WebSocket::EvaluateRequest(const RestInputStruct &REQ){
  RestOutputStruct out;
    out.in_struct = REQ;
  QHostAddress host;
    if(SOCKET!=0){ host = SOCKET->peerAddress(); }
    else if(TSOCKET!=0){ host = TSOCKET->peerAddress(); }
  if(!REQ.VERB.isEmpty() && REQ.VERB != "GET" && REQ.VERB!="POST" && REQ.VERB!="PUT"){
    //Non-supported request (at the moment) - return an error message
    out.CODE = RestOutputStruct::BADREQUEST;
  }else if(out.in_struct.name.isEmpty() || out.in_struct.namesp.isEmpty() ){
    //Invalid JSON structure validity
    //Note: id and args are optional at this stage - let the subsystems handle those inputs
    out.CODE = RestOutputStruct::BADREQUEST;
  }else{
    //First check for a REST authorization (not stand-alone request)
    if(!out.in_struct.auth.isEmpty()){
      AUTHSYSTEM->clearAuth(SockAuthToken); //new auth requested - clear any old token
      SockAuthToken = AUTHSYSTEM->LoginUP(host, out.in_struct.auth.section(":",0,0), out.in_struct.auth.section(":",1,1));
    }
	  
    //Now check the body of the message and do what it needs
      if(out.in_struct.namesp.toLower() == "rpc"){
	if(out.in_struct.name.startsWith("auth")){
	  //Now perform authentication based on type of auth given
	  //Note: This sets/changes the current SockAuthToken
	  AUTHSYSTEM->clearAuth(SockAuthToken); //new auth requested - clear any old token
	  if(DEBUG){ qDebug() << "Authenticate Peer:" << SOCKET->peerAddress().toString(); }
	  //Now do the auth
	  if(out.in_struct.name=="auth" && out.in_struct.args.isObject() ){
	    //username/[password/cert] authentication
	    QString user, pass;
	    if(out.in_struct.args.toObject().contains("username")){ user = JsonValueToString(out.in_struct.args.toObject().value("username"));  }
	    if(out.in_struct.args.toObject().contains("password")){ pass = JsonValueToString(out.in_struct.args.toObject().value("password"));  }
	    if(!pass.isEmpty()){
	      //Use the given password
	      SockAuthToken = AUTHSYSTEM->LoginUP(host, user, pass);
	    }else{
	      //No password - use the current SSL certificates instead
	      QList<QSslCertificate> certs;
	      if(SOCKET!=0){ certs = SOCKET->sslConfiguration().peerCertificateChain(); }
	      else if(TSOCKET!=0){ certs = TSOCKET->peerCertificateChain(); }
	      SockAuthToken = AUTHSYSTEM->LoginUC(host, user, certs);
	    }
	  }else if(out.in_struct.name == "auth_token" && out.in_struct.args.isObject()){
	    SockAuthToken = JsonValueToString(out.in_struct.args.toObject().value("token"));
	  }else if(out.in_struct.name == "auth_clear"){
	    return; //don't send a return message after clearing an auth (already done)
	  }
	  
	  //Now check the auth and respond appropriately
	  if(AUTHSYSTEM->checkAuth(SockAuthToken)){
	    //Good Authentication - return the new token 
	    QJsonArray array;
	      array.append(SockAuthToken);
	      array.append(AUTHSYSTEM->checkAuthTimeoutSecs(SockAuthToken));
	    out.out_args = array;
	    out.CODE = RestOutputStruct::OK;
	  }else{
	    if(SockAuthToken=="REFUSED"){
	      out.CODE = RestOutputStruct::FORBIDDEN;
	    }
	    SockAuthToken.clear(); //invalid token
	    //Bad Authentication - return error
	      out.CODE = RestOutputStruct::UNAUTHORIZED;
	  }
		
	}else if( AUTHSYSTEM->checkAuth(SockAuthToken) ){ //validate current Authentication token	 
	  //Now provide access to the various subsystems
	  // First get/set the permissions flag into the input structure
	    out.in_struct.fullaccess = AUTHSYSTEM->hasFullAccess(SockAuthToken);
	  //Pre-set any output fields
          QJsonObject outargs;	
	    out.CODE = EvaluateBackendRequest(out.in_struct, &outargs);
            out.out_args = outargs;	  
        }else{
	  //Bad/No authentication
	  out.CODE = RestOutputStruct::UNAUTHORIZED;
	}
	    	
      }else if(out.in_struct.namesp.toLower() == "events"){
          if( AUTHSYSTEM->checkAuth(SockAuthToken) ){ //validate current Authentication token	 
	    //Pre-set any output fields
            QJsonObject outargs;	
	    //Assemble the list of input events
	    QStringList evlist;
	    if(out.in_struct.args.isObject()){ evlist << JsonValueToString(out.in_struct.args); }
	    else if(out.in_struct.args.isArray()){ evlist = JsonArrayToStringList(out.in_struct.args.toArray()); }
	    //Now subscribe/unsubscribe to these events
	    int sub = -1; //bad input
	    if(out.in_struct.name=="subscribe"){ sub = 1; }
	    else if(out.in_struct.name=="unsubscribe"){ sub = 0; }
	    //qDebug() << "Got Client Event Modification:" << sub << evlist;
	    if(sub>=0 && !evlist.isEmpty() ){
	      for(int i=0; i<evlist.length(); i++){
	        EventWatcher::EVENT_TYPE type = EventWatcher::typeFromString(evlist[i]);
		if(type==EventWatcher::BADEVENT){ continue; }
		outargs.insert(out.in_struct.name,QJsonValue(evlist[i]));
		if(sub==1){ 
		  ForwardEvents << type; 
		  EventUpdate(type);
		}else{
		  ForwardEvents.removeAll(type);
		}
	      }
	      out.out_args = outargs;
	      out.CODE = RestOutputStruct::OK;
	    }else{
	      //Bad/No authentication
	      out.CODE = RestOutputStruct::BADREQUEST;		    
	    }
          }else{
	    //Bad/No authentication
	    out.CODE = RestOutputStruct::UNAUTHORIZED;
	  }
	//Other namespace - check whether auth has already been established before continuing
	}else if( AUTHSYSTEM->checkAuth(SockAuthToken) ){ //validate current Authentication token	 
	  //Now provide access to the various subsystems
	  // First get/set the permissions flag into the input structure
	  out.in_struct.fullaccess = AUTHSYSTEM->hasFullAccess(SockAuthToken);
	  //Pre-set any output fields
          QJsonObject outargs;	
	    out.CODE = EvaluateBackendRequest(out.in_struct, &outargs);
            out.out_args = outargs;
	}else{
	  //Error in inputs - assemble the return error message
	  out.CODE = RestOutputStruct::UNAUTHORIZED;
	}
    //If this is a REST input - go ahead and format the output header
    if(out.CODE == RestOutputStruct::OK){
      out.Header << "Content-Type: text/json; charset=utf-8";
    }
  }
  //Return any information
  this->sendReply(out.assembleMessage());
  if(out.CODE == RestOutputStruct::FORBIDDEN && SOCKET!=0){
    SOCKET->close(QWebSocketProtocol::CloseCodeNormal, "Too Many Authorization Failures - Try again later");
  }
}

// === GENERAL PURPOSE UTILITY FUNCTIONS ===
QString WebSocket::JsonValueToString(QJsonValue val){
  //Note: Do not use this on arrays - only use this on single-value values
  QString out;
  switch(val.type()){
    case QJsonValue::Bool:
	out = (val.toBool() ? "true": "false"); break;
    case QJsonValue::Double:
	out = QString::number(val.toDouble()); break;
    case QJsonValue::String:
	out = val.toString(); break;
    case QJsonValue::Array:
	out = "\""+JsonArrayToStringList(val.toArray()).join("\" \"")+"\"";
    default:
	out.clear();
  }
  return out;
}

QStringList WebSocket::JsonArrayToStringList(QJsonArray array){
  //Note: This assumes that the array is only values, not additional objects
  QStringList out;
  if(DEBUG){ qDebug() << "Array to List:" << array.count(); }
  for(int i=0; i<array.count(); i++){
    out << JsonValueToString(array.at(i));
  }
  return out;  
}

// =====================
//       PRIVATE SLOTS
// =====================
void WebSocket::checkIdle(){
  if(SOCKET !=0){
    LogManager::log(LogManager::HOST,"Connection Idle: "+SockPeerIP);
    SOCKET->close(); //timeout - close the connection to make way for others
  }
  if(TSOCKET !=0){
    LogManager::log(LogManager::HOST,"Connection Idle: "+SockPeerIP);
    TSOCKET->close(); //timeout - close the connection to make way for others
  }
}

void WebSocket::SocketClosing(){
  LogManager::log(LogManager::HOST,"Connection Closing: "+SockPeerIP);
  if(idletimer->isActive()){ 
    //This means the client deliberately closed the connection - not the idle timer
    //qDebug() << " - Client Closed Connection";
    idletimer->stop(); 
  }else{
    //qDebug() << "idleTimer not running";
  }
  //Stop any current requests

  //Reset the pointer
  if(SOCKET!=0){ SOCKET = 0;	 }
  if(TSOCKET!=0){ TSOCKET = 0; }
  
  emit SocketClosed(SockID);
}

void WebSocket::EvaluateMessage(const QByteArray &msg){
  //qDebug() << "New Binary Message:";
  if(idletimer->isActive()){ idletimer->stop(); }
  idletimer->start(); 
  EvaluateREST( QString(msg) );
  //qDebug() << " - Done with Binary Message";
}

void WebSocket::EvaluateMessage(const QString &msg){ 
  //qDebug() << "New Text Message:";
  if(idletimer->isActive()){ idletimer->stop(); }
  idletimer->start(); 
  EvaluateREST(msg); 
  //qDebug() << " - Done with Text Message";
}

void WebSocket::EvaluateTcpMessage(){
  //Need to read the data from the Tcp socket and turn it into a string
  //qDebug() << "New TCP Message:";
  if(idletimer->isActive()){ idletimer->stop(); }
  QString msg = QString(TSOCKET->readAll());
  for(int i=0; i<5 && !msg.endsWith("}"); i++){
    usleep(10000); //10ms
    msg.append( QString(TSOCKET->readAll()) );
  }
  EvaluateREST(msg );
  idletimer->start(); 
  //qDebug() << " - Done with TCP Message";	
}

//SSL signal handling
void WebSocket::nowEncrypted(){
  //the socket/connection is now encrypted
  //qDebug() << " - Socket now encrypted";
}

void WebSocket::peerError(const QSslError&){ //peerVerifyError() signal
  //qDebug() << "Socket Peer Error:";
}

void WebSocket::SslError(const QList<QSslError> &err){ //sslErrors() signal
  LogManager::log(LogManager::HOST,"Connection SSL Errors ["+SockPeerIP+"]: "+err.length());
}

// ======================
//       PUBLIC SLOTS
// ======================
void WebSocket::EventUpdate(EventWatcher::EVENT_TYPE evtype, QJsonValue msg){
  if(msg.isNull()){ msg = EVENTS->lastEvent(evtype); }
  //qDebug() << "Socket Status Update:" << msg;
  if(!ForwardEvents.contains(evtype)){ return; }
  RestOutputStruct out;
    out.CODE = RestOutputStruct::OK;
    out.in_struct.namesp = "events";
    out.out_args = msg;
    out.Header << "Content-Type: text/json; charset=utf-8"; //REST header info
    out.in_struct.name = EventWatcher::typeToString(evtype);
  
  //Now send the message back through the socket
  this->sendReply(out.assembleMessage());
}
