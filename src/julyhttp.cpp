// Copyright (C) 2013 July IGHOR.
// I want to create trading application that can be configured for any rule and strategy.
// If you want to help me please Donate: 1d6iMwjjNo8ZGYeJBZKXgcgVk9o7fXcjc
// For any questions please use contact form https://sourceforge.net/projects/bitcointrader/
// Or send e-mail directly to julyighor@gmail.com
//
// You may use, distribute and copy the Qt Bitcion Trader under the terms of
// GNU General Public License version 3

#include "julyhttp.h"
#include "main.h"
#include <QTimer>
#include <zlib.h>
#include <QFile>
#include <QMutex>
#include <QWaitCondition>

JulyHttp::JulyHttp(const QString &hostN, const QByteArray &restLine, QObject *parent, const bool &secure, const bool &keepAlive, const QByteArray &contentType)
	: QSslSocket(parent)
{
	secureConnection=secure;
	isDataPending=false;
	httpState=999;
	connectionClose=false;
	bytesDone=0;
	contentLength=0;
	chunkedSize=-1;
	readingHeader=false;
	waitingReplay=false;
	isDisabled=false;
	outGoingPacketsCount=0;
	contentGzipped=false;

	setupSocket();
	
	requestTimeOut.restart();
	hostName=hostN;
	httpHeader.append(" HTTP/1.1\r\n");
	if(baseValues.customUserAgent.length()>0)
	httpHeader.append("User-Agent: "+baseValues.customUserAgent+"\r\n");
		else
	httpHeader.append("User-Agent: Qt Bitcoin Trader v"+baseValues.appVerStr+"\r\n");
	httpHeader.append("Host: "+hostName+"\r\n");
	if(baseValues.gzipEnabled)httpHeader.append("Accept-Encoding: gzip\r\n");
	httpHeader.append("Content-Type: "+contentType+"\r\n");
	if(keepAlive)httpHeader.append("Connection: keep-alive\r\n");
	else httpHeader.append("Connection: close\r\n");
	apiDownState=false;
	apiDownCounter=0;
	restKeyLine=restLine;

	if(baseValues.customCookies.length()>0)
	{
		lastCookie="SetCookie: "+baseValues.customCookies.toAscii()+"\r\n";
		QStringList cookieListStr=baseValues.customCookies.split("; ");
		for(int n=0;n<cookieListStr.count();n++)
		{
			QStringList nameValue=cookieListStr.at(n).split("=");
			if(nameValue.count()!=2)continue;
			cookiesList<<QNetworkCookie(nameValue.first().toAscii(),nameValue.last().toAscii());
		}
	}

	QTimer *secondTimer=new QTimer(this);
	connect(secondTimer,SIGNAL(timeout()),this,SLOT(sendPendingData()));
	secondTimer->start(300);
}

JulyHttp::~JulyHttp()
{
	abortSocket();
}

void JulyHttp::setupSocket()
{
	static QList<QSslCertificate> certs;
	if(certs.count()==0)
	{
		QFile readCerts(":/Resources/CertBase.cer");
		if(readCerts.open(QIODevice::ReadOnly))
		{
			QByteArray certData=readCerts.readAll()+"{SPLIT}";
			readCerts.close();
			do 
			{
			int nextCert=certData.indexOf("{SPLIT}");
			if(nextCert>-1)
			{
				QByteArray currentCert=certData.left(nextCert);
				QSslCertificate derCert(currentCert,QSsl::Der);
				if(derCert.isValid())certs<<derCert;
				certData.remove(0,nextCert+7);
			}
			else certData.clear();
			}while(certData.size());
		}
	}

	if(certs.count())
	{
	addCaCertificates(certs);
	addDefaultCaCertificates(certs);
	}

	setPeerVerifyMode(QSslSocket::VerifyPeer);
	setSocketOption(QAbstractSocket::KeepAliveOption,true);
	connect(this,SIGNAL(readyRead()),SLOT(readSocket()));
	connect(this,SIGNAL(error(QAbstractSocket::SocketError)),this,SLOT(errorSlot(QAbstractSocket::SocketError)));
	connect(this,SIGNAL(sslErrors(const QList<QSslError> &)),this,SLOT(sslErrorsSlot(const QList<QSslError> &)));
}

void JulyHttp::clearPendingData()
{
	for(int n=requestList.count()-1;n>=0;n--)takeRequestAt(n);
	reConnect();
}

void JulyHttp::reConnect(bool mastAbort)
{
	if(isDisabled)return;
	reconnectSocket(mastAbort);
	retryRequest();
}

void JulyHttp::abortSocket()
{
	blockSignals(true);
	abort();
	blockSignals(false);
}

void JulyHttp::reconnectSocket(bool mastAbort)
{
	if(isDisabled)return;
	if(mastAbort)abortSocket();
	if(state()==QAbstractSocket::UnconnectedState||state()==QAbstractSocket::UnconnectedState)
	{
		if(secureConnection)connectToHostEncrypted(hostName, 443, QIODevice::ReadWrite);
		else connectToHost(hostName,80,QIODevice::ReadWrite);
	}
}

void JulyHttp::setApiDown(bool httpError)
{
	if(httpError)apiDownCounter++;else apiDownCounter=0;

	bool currentApiDownState=apiDownCounter>baseValues.apiDownCount;
	if(apiDownState!=currentApiDownState)
	{
		apiDownState=currentApiDownState;
		emit apiDown(apiDownState);
	}
}

void JulyHttp::updateCookiesFromLastCookie()
{
	QByteArray currentCookie=lastCookie;
	int currentCookiesCount=cookiesList.count();
	QList<QNetworkCookie> newCookedList=QNetworkCookie::parseCookies(currentCookie);
	for(int k=0;k<newCookedList.count();k++)
	{
		bool updated=false;
		for(int n=currentCookiesCount-1;n>=0;n--)
			if(newCookedList.at(k).name()==cookiesList.at(n).name())
			{
				updated=true;
				cookiesList[n]=newCookedList.at(k);
			}
			if(!updated)cookiesList<<newCookedList.at(k);
	}

	cookieLine.clear();
	for(int n=0;n<cookiesList.count();n++)
	{
		if(cookiesList.at(n).value().isEmpty())continue;
		QByteArray currentParsedCookie=cookiesList.at(n).toRawForm(QNetworkCookie::NameAndValueOnly);
		if(currentParsedCookie.size()<=12)continue;
		if(currentParsedCookie.toLower().startsWith("set-cookie: "))
			currentParsedCookie.remove(0,12);
		cookieLine.append(currentParsedCookie);
		if(n<cookiesList.count()-1)cookieLine.append("; ");
	}
	if(!cookieLine.isEmpty()){cookieLine.prepend("Cookie: ");cookieLine.append("\r\n");}
}

void JulyHttp::readSocket()
{
	if(isDisabled)return;

	requestTimeOut.restart();

	if(!waitingReplay)
	{
		httpState=999;
		bytesDone=0;
		connectionClose=false;
		buffer.clear();
		contentGzipped=false;
		waitingReplay=true;
		readingHeader=true;
		contentLength=0;
	}

	while(readingHeader)
	{
		bool endFound=false;
		QByteArray currentLine;
		while(!endFound&&canReadLine())
		{
			currentLine=readLine();
			if(currentLine=="\r\n"||
			   currentLine=="\n"||
			   currentLine.isEmpty())endFound=true;
			else
			{
				QString currentLineLow=currentLine.toLower();
				if(currentLineLow.startsWith("http/1.1 "))
				{
					if(currentLineLow.length()>12)httpState=currentLineLow.mid(9,3).toInt();
					if(debugLevel)
					{
						if(httpState!=200)logThread->writeLog(currentLine+readAll());
					}
				}
				else
				if(currentLineLow.startsWith("set-cookie"))
				{
					if(lastCookie!=currentLine)
					{
					lastCookie=currentLine;
					updateCookiesFromLastCookie();
					}
				}
				else 
				if(currentLineLow.startsWith("transfer-encoding")&&
				   currentLineLow.endsWith("chunked\r\n"))chunkedSize=0;
				else
				if(currentLineLow.startsWith(QLatin1String("content-length")))
				{
					QStringList pairList=currentLineLow.split(":");
					if(pairList.count()==2)contentLength=pairList.last().trimmed().toUInt();
				}
				else
				if(currentLineLow.startsWith(QLatin1String("connection"))&&
					currentLineLow.endsWith(QLatin1String("close\r\n")))connectionClose=true;
				else
				if(currentLineLow.startsWith(QLatin1String("content-encoding"))&&
					currentLineLow.contains(QLatin1String("gzip")))contentGzipped=true;
			}
		}
		if(!endFound)
		{
			retryRequest();
			return;
		}
		readingHeader=false;
	}
	if(httpState<400)emit anyDataReceived();

	bool allDataReaded=false;

		qint64 readSize=bytesAvailable();

		QByteArray *dataArray=0;
		if(chunkedSize!=-1)
		{
			while(true)
			{
				if(chunkedSize==0)
				{
					if(!canReadLine())break;
					QString sizeString=readLine();
					int tPos=sizeString.indexOf(QLatin1Char(';'));
					if(tPos!=-1)sizeString.truncate(tPos);
					bool ok;
					chunkedSize=sizeString.toInt(&ok,16);
					if(!ok)
					{
						if(debugLevel)logThread->writeLog("Invalid size",2);
						if(dataArray){delete dataArray;dataArray=0;}
						retryRequest();
						return;
					}
					if(chunkedSize==0)chunkedSize=-2;
				}

				while(chunkedSize==-2&&canReadLine())
				{
					QString currentLine=readLine();
					 if(currentLine==QLatin1String("\r\n")||
						currentLine==QLatin1String("\n"))chunkedSize=-1;
				}
				if(chunkedSize==-1)
				{
					allDataReaded=true;
					break;
				}

				readSize=bytesAvailable();
				if(readSize==0)break;
				if(readSize==chunkedSize||readSize==chunkedSize+1)
				{
					readSize=chunkedSize-1;
					if(readSize==0)break;
				}

				qint64 bytesToRead=chunkedSize<0?readSize:qMin(readSize,chunkedSize);
				if(!dataArray)dataArray=new QByteArray;
				quint32 oldDataSize=dataArray->size();
				dataArray->resize(oldDataSize+bytesToRead);
				qint64 read=this->read(dataArray->data()+oldDataSize,bytesToRead);
				dataArray->resize(oldDataSize+read);

				chunkedSize-=read;
				if(chunkedSize==0&&readSize-read>=2)
				{
					char twoBytes[2];
					this->read(twoBytes,2);
					if(twoBytes[0]!='\r'||twoBytes[1]!='\n')
					{
						if(debugLevel)logThread->writeLog("Invalid HTTP chunked body",2);
						if(dataArray){delete dataArray;dataArray=0;}
						retryRequest();
						return;
					}
				}
			}
		} 
		else
			if(contentLength>0)
			{
			readSize=qMin(qint64(contentLength-bytesDone),readSize);
			if(readSize>0)
			{
				if(dataArray){delete dataArray;dataArray=0;}
				dataArray=new QByteArray;
				dataArray->resize(readSize);
				dataArray->resize(read(dataArray->data(),readSize));
			}
			if(bytesDone+bytesAvailable()+readSize==contentLength)allDataReaded=true;
			}
			else 
			if(readSize>0)
			{
			if(!dataArray)dataArray=new QByteArray(readAll());
			}

		if(dataArray)
		{
				readSize=dataArray->size();
				if(readSize>0)buffer.append(*dataArray);
				if(dataArray){delete dataArray;dataArray=0;}
				if(contentLength>0)
				{
					bytesDone+=readSize;
					emit dataProgress(100*bytesDone/contentLength);
				}
		}
		if(dataArray){delete dataArray;dataArray=0;}

	if(allDataReaded)
	{
		if(!buffer.isEmpty()&&requestList.count())
		{
			if(contentGzipped)uncompress(&buffer);
			bool apiMaybeDown=buffer[0]=='<';
			setApiDown(apiMaybeDown);
			if(debugLevel&&buffer.isEmpty())logThread->writeLog("Response is EMPTY",2);
			if(!apiMaybeDown)emit dataReceived(buffer,requestList.first().reqType);
		}
		waitingReplay=false;
		readingHeader=true;
		if(!buffer.isEmpty())
		{
		if(requestList.count())requestList[0].retryCount=0;

		takeFirstRequest();
		clearRequest();
		if(connectionClose)
		{
			if(debugLevel)logThread->writeLog("HTTP: connection closed");
			reConnect(true);
		}
		}
		sendPendingData();
	}
}

void JulyHttp::uncompress(QByteArray *data)
{
	if(data->size()<=4)
	{
		if(debugLevel)logThread->writeLog("GZIP: Input data is truncated",2);
		return;
	}

	QByteArray result;

	static const int CHUNK_SIZE=1024;
	char out[CHUNK_SIZE];

	z_stream strm;
	strm.zalloc=Z_NULL;
	strm.zfree=Z_NULL;
	strm.opaque=Z_NULL;
	strm.avail_in=data->size();
	strm.next_in=(Bytef*)(data->data());

	int ret=inflateInit2(&strm,47);
	if(ret!=Z_OK)return;

	do
	{
		strm.avail_out=CHUNK_SIZE;
		strm.next_out=(Bytef*)(out);

		ret=inflate(&strm,Z_NO_FLUSH);
		Q_ASSERT(ret!=Z_STREAM_ERROR);
		switch(ret)
		{
		case Z_NEED_DICT: ret=Z_DATA_ERROR;
		case Z_DATA_ERROR:
		case Z_MEM_ERROR: (void)inflateEnd(&strm);
			return;
		}
		result.append(out, CHUNK_SIZE-strm.avail_out);
	} 
	while(strm.avail_out==0);

	inflateEnd(&strm);
	(*data)=result;
}

bool JulyHttp::isReqTypePending(int val)
{
	return reqTypePending.value(val,0)>0;
}

void JulyHttp::retryRequest()
{
	if(isDisabled||requestList.count()==0)return;
	if(requestList.first().retryCount<=0)takeFirstRequest();
	else
	{
		if(debugLevel)logThread->writeLog("Warning: Request resent due timeout",2);
		requestList[0].retryCount--;
	}
	sendPendingData();
}

void JulyHttp::clearRequest()
{
	buffer.clear();
	chunkedSize=-1;
	nextPacketMastBeSize=false;
	endOfPacket=false;
}

void JulyHttp::prepareData(int reqType, const QByteArray &method, QByteArray postData, const QByteArray &restSignLine, const int &forceRetryCount)
{
	if(isDisabled)return;
	QByteArray *data=new QByteArray(method+httpHeader+cookieLine);
	if(!restSignLine.isEmpty())data->append(restKeyLine+restSignLine);
	if(!postData.isEmpty())
	{
		data->append("Content-Length: "+QByteArray::number(postData.size())+"\r\n\r\n");
		data->append(postData);
	}
	else data->append("\r\n");

	PacketItem newPacket;
			   newPacket.data=data;
			   newPacket.reqType=reqType;
			   newPacket.retryCount=0;
			   newPacket.skipOnce=false;

	if(forceRetryCount==-1)
	{
		if(reqType>300)newPacket.retryCount=baseValues.httpRetryCount-1;
	}
	else newPacket.retryCount=forceRetryCount;
	reqTypePending[reqType]=reqTypePending.value(reqType,0)+1;

	preparedList<<newPacket;
}

void JulyHttp::prepareDataSend()
{
	if(isDisabled)return;
	if(preparedList.count()==0)return;

	for(int n=1;n<preparedList.count();n++)
	{
		preparedList[0].data->append(*(preparedList[n].data))+"\r\n\r\n";
		preparedList[n].skipOnce=true;
	}
	for(int n=0;n<preparedList.count();n++)requestList<<preparedList.at(n);
	preparedList.clear();
	if(isDataPending!=true)
	{
		emit setDataPending(true);
		isDataPending=true;
	}
}

void JulyHttp::prepareDataClear()
{
	if(isDisabled)return;
	for(int n=0;n<preparedList.count();n++)
	{
		PacketItem preparingPacket=preparedList.at(n);
		reqTypePending[preparingPacket.reqType]=reqTypePending.value(preparingPacket.reqType,1)-1;
		if(preparingPacket.data)delete preparingPacket.data;
	}
	preparedList.clear();
}

void JulyHttp::sendData(int reqType, const QByteArray &method, QByteArray postData, const QByteArray &restSignLine, const int &forceRetryCount)
{
	if(isDisabled)return;
	QByteArray *data=new QByteArray(method+httpHeader+cookieLine);
	if(!restSignLine.isEmpty())data->append(restKeyLine+restSignLine);
	if(!postData.isEmpty())
	{
		data->append("Content-Length: "+QByteArray::number(postData.size())+"\r\n\r\n");
		data->append(postData);
	}
	else data->append("\r\n");

	if(reqType>300)
		for(int n=requestList.count()-1;n>=1;n--)
			if(requestList.at(n).reqType<300&&requestList[n].skipOnce!=true)
				takeRequestAt(n);

	PacketItem newPacket;
	           newPacket.data=data;
	           newPacket.reqType=reqType;
	           newPacket.retryCount=0;
	           newPacket.skipOnce=false;

	if(forceRetryCount==-1)
	{
	if(reqType>300)newPacket.retryCount=baseValues.httpRetryCount-1;
	else newPacket.retryCount=0;
	}
	else newPacket.retryCount=forceRetryCount;

	if(newPacket.retryCount>0&&debugLevel&&newPacket.reqType>299)logThread->writeLog("Added to Query RetryCount="+QByteArray::number(newPacket.retryCount),2);
	requestList<<newPacket;

	if(isDataPending!=true)
	{
		emit setDataPending(true);
		isDataPending=true;
	}

	reqTypePending[reqType]=reqTypePending.value(reqType,0)+1;
	sendPendingData();
}

void JulyHttp::takeRequestAt(int pos)
{
	if(requestList.count()<=pos)return;
	PacketItem packetTake=requestList.at(pos);
	reqTypePending[packetTake.reqType]=reqTypePending.value(packetTake.reqType,1)-1;

	delete packetTake.data;
	packetTake.data=0;
	requestList.removeAt(pos);

	if(requestList.count()==0)
	{
		reqTypePending.clear();

		if(isDataPending!=false)
		{
			emit setDataPending(false);
			isDataPending=false;
		}
	}
}

void JulyHttp::takeFirstRequest()
{
	if(requestList.count()==0)return;
	takeRequestAt(0);
}

void JulyHttp::errorSlot(QAbstractSocket::SocketError socketError)
{
	if(socketError!=QAbstractSocket::RemoteHostClosedError||socketError!=QAbstractSocket::UnfinishedSocketOperationError)setApiDown(true);

	if(debugLevel)logThread->writeLog("SocketError: "+errorString().toAscii(),2);

	if(socketError==QAbstractSocket::ProxyAuthenticationRequiredError)
	{
		isDisabled=true;
		emit errorSignal(errorString());
		abortSocket();
	}
	else
	{
		QMutex mutex;
		mutex.lock();

		QWaitCondition waitCondition;
		waitCondition.wait(&mutex, 1000);

		mutex.unlock();

		reconnectSocket(false);
	}
}

bool JulyHttp::isSocketConnected()
{
	return state()==QAbstractSocket::ConnectedState;
}

void JulyHttp::sendPendingData()
{
	if(isDisabled)return;
	if(requestList.count()==0)return;

	if(!isSocketConnected())reconnectSocket(false);

	if(state()!=QAbstractSocket::UnconnectedState)
	{
		if(state()==QAbstractSocket::ConnectingState||state()==QAbstractSocket::HostLookupState)waitForConnected(baseValues.httpRequestTimeout+1000);
	}
	if(!isSocketConnected())
	{
		setApiDown(true);
		if(debugLevel)logThread->writeLog("Socket state: "+errorString().toAscii(),2);
		reconnectSocket(false);
		if(state()==QAbstractSocket::ConnectingState)waitForConnected(baseValues.httpRequestTimeout+1000);
	}
	else reconnectSocket(false);

	if(!isSocketConnected())return;

	if(currentPendingRequest==requestList.first().data)
	{
		if(requestTimeOut.elapsed()<baseValues.httpRequestTimeout)return;
		else
		{
			if(debugLevel)logThread->writeLog(QString("Request timeout: %0>%1").arg(requestTimeOut.elapsed()).arg(baseValues.httpRequestTimeout).toAscii(),2);
			reconnectSocket(true);
			setApiDown(true);
			if(requestList.first().retryCount>0){retryRequest();return;}
		}
	}
	else
	{
		currentPendingRequest=requestList.first().data;
		if(debugLevel&&requestList.first().reqType>299)logThread->writeLog("Sending request ID: "+QByteArray::number(requestList.first().reqType),2);
	}
	clearRequest();

	requestTimeOut.restart();
	if(debugLevel)logThread->writeLog("SND: "+QByteArray(*currentPendingRequest).replace(baseValues.restKey,"REST_KEY").replace(baseValues.restSign,"REST_SIGN"));

	if(currentPendingRequest)
	{
		if(requestList.first().skipOnce==true)requestList[0].skipOnce=false;
		else
		{
			if(bytesAvailable())
			{
				if(debugLevel)logThread->writeLog("Cleared previous data: "+readAll());
				else readAll();
			}
			waitingReplay=false;
			write(*currentPendingRequest);
			flush();
		}
	}
	else if(debugLevel)logThread->writeLog("PendingRequest pointer not exist",2);
}

void JulyHttp::sslErrorsSlot(const QList<QSslError> &val)
{
	for(int n=0;n<val.count();n++)
		if(val.at(n).error()==QAbstractSocket::SocketTimeoutError)
		{
			requestTimeOut.addMSecs(baseValues.httpRequestTimeout);
			sendPendingData();
			setApiDown(true);
			break;
		}
	emit sslErrorSignal(val);
}