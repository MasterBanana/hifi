//
//  ScriptEngine.cpp
//  libraries/script-engine/src
//
//  Created by Brad Hefta-Gaub on 12/14/13.
//  Copyright 2013 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include <QtCore/QCoreApplication>
#include <QtCore/QEventLoop>
#include <QtCore/QTimer>
#include <QtCore/QThread>
#include <QtNetwork/QNetworkRequest>
#include <QtNetwork/QNetworkReply>
#include <QScriptEngine>

#include <AudioConstants.h>
#include <AudioEffectOptions.h>
#include <AvatarData.h>
#include <CollisionInfo.h>
#include <EntityScriptingInterface.h>
#include <NetworkAccessManager.h>
#include <NodeList.h>
#include <PacketHeaders.h>
#include <UUID.h>

#include "AnimationObject.h"
#include "ArrayBufferViewClass.h"
#include "BatchLoader.h"
#include "DataViewClass.h"
#include "EventTypes.h"
#include "MenuItemProperties.h"
#include "ScriptAudioInjector.h"
#include "ScriptCache.h"
#include "ScriptEngine.h"
#include "TypedArrays.h"
#include "XMLHttpRequestClass.h"

#include "SceneScriptingInterface.h"

#include "MIDIEvent.h"


static QScriptValue debugPrint(QScriptContext* context, QScriptEngine* engine){
    qDebug() << "script:print()<<" << context->argument(0).toString();
    QString message = context->argument(0).toString()
        .replace("\\", "\\\\")
        .replace("\n", "\\n")
        .replace("\r", "\\r")
        .replace("'", "\\'");
    engine->evaluate("Script.print('" + message + "')");
    return QScriptValue();
}

QScriptValue avatarDataToScriptValue(QScriptEngine* engine, AvatarData* const &in) {
    return engine->newQObject(in);
}

void avatarDataFromScriptValue(const QScriptValue &object, AvatarData* &out) {
    out = qobject_cast<AvatarData*>(object.toQObject());
}

QScriptValue inputControllerToScriptValue(QScriptEngine *engine, AbstractInputController* const &in) {
    return engine->newQObject(in);
}

void inputControllerFromScriptValue(const QScriptValue &object, AbstractInputController* &out) {
    out = qobject_cast<AbstractInputController*>(object.toQObject());
}

ScriptEngine::ScriptEngine(const QString& scriptContents, const QString& fileNameString,
                           AbstractControllerScriptingInterface* controllerScriptingInterface) :

    _scriptContents(scriptContents),
    _isFinished(false),
    _isRunning(false),
    _isInitialized(false),
    _isAvatar(false),
    _avatarIdentityTimer(NULL),
    _avatarBillboardTimer(NULL),
    _timerFunctionMap(),
    _isListeningToAudioStream(false),
    _avatarSound(NULL),
    _numAvatarSoundSentBytes(0),
    _controllerScriptingInterface(controllerScriptingInterface),
    _avatarData(NULL),
    _scriptName(),
    _fileNameString(fileNameString),
    _quatLibrary(),
    _vec3Library(),
    _uuidLibrary(),
    _isUserLoaded(false),
    _arrayBufferClass(new ArrayBufferClass(this))
{
    _allScriptsMutex.lock();
    _allKnownScriptEngines.insert(this);
    _allScriptsMutex.unlock();
}

ScriptEngine::~ScriptEngine() {
    // If we're not already in the middle of stopping all scripts, then we should remove ourselves
    // from the list of running scripts. We don't do this if we're in the process of stopping all scripts
    // because that method removes scripts from its list as it iterates them
    if (!_stoppingAllScripts) {
        _allScriptsMutex.lock();
        _allKnownScriptEngines.remove(this);
        _allScriptsMutex.unlock();
    }
}

QSet<ScriptEngine*> ScriptEngine::_allKnownScriptEngines;
QMutex ScriptEngine::_allScriptsMutex;
bool ScriptEngine::_stoppingAllScripts = false;
bool ScriptEngine::_doneRunningThisScript = false;

void ScriptEngine::stopAllScripts(QObject* application) {
    _allScriptsMutex.lock();
    _stoppingAllScripts = true;
    
    QMutableSetIterator<ScriptEngine*> i(_allKnownScriptEngines);
    while (i.hasNext()) {
        ScriptEngine* scriptEngine = i.next();

        QString scriptName = scriptEngine->getFilename();

        // NOTE: typically all script engines are running. But there's at least one known exception to this, the
        // "entities sandbox" which is only used to evaluate entities scripts to test their validity before using
        // them. We don't need to stop scripts that aren't running. 
        if (scriptEngine->isRunning()) {
        
            // If the script is running, but still evaluating then we need to wait for its evaluation step to
            // complete. After that we can handle the stop process appropriately
            if (scriptEngine->evaluatePending()) {
                while (scriptEngine->evaluatePending()) {
                
                    // This event loop allows any started, but not yet finished evaluate() calls to complete
                    // we need to let these complete so that we can be guaranteed that the script engine isn't
                    // in a partially setup state, which can confuse our shutdown unwinding.
                    QEventLoop loop;
                    QObject::connect(scriptEngine, &ScriptEngine::evaluationFinished, &loop, &QEventLoop::quit);
                    loop.exec();
                }
            }

            // We disconnect any script engine signals from the application because we don't want to do any 
            // extra stopScript/loadScript processing that the Application normally does when scripts start
            // and stop. We can safely short circuit this because we know we're in the "quitting" process
            scriptEngine->disconnect(application);
            
            // Calling stop on the script engine will set it's internal _isFinished state to true, and result
            // in the ScriptEngine gracefully ending it's run() method.
            scriptEngine->stop();

            // We need to wait for the engine to be done running before we proceed, because we don't
            // want any of the scripts final "scriptEnding()" or pending "update()" methods from accessing
            // any application state after we leave this stopAllScripts() method
            scriptEngine->waitTillDoneRunning();
            
            // If the script is stopped, we can remove it from our set
            i.remove();
        }
    }
    _stoppingAllScripts = false;
    _allScriptsMutex.unlock();
}


void ScriptEngine::waitTillDoneRunning() {
    QString scriptName = getFilename();

    // If the script never started running or finished running before we got here, we don't need to wait for it
    if (_isRunning) {

        _doneRunningThisScript = false; // NOTE: this is static, we serialize our waiting for scripts to finish
        
        // NOTE: waitTillDoneRunning() will be called on the main Application thread, inside of stopAllScripts()
        // we want the application thread to continue to process events, because the scripts will likely need to 
        // marshall messages across to the main thread. For example if they access Settings or Meny in any of their
        // shutdown code.
        while (!_doneRunningThisScript) {
            
            // process events for the main application thread, allowing invokeMethod calls to pass between threads
            QCoreApplication::processEvents();
        }
    }
}

QString ScriptEngine::getFilename() const { 
    QStringList fileNameParts = _fileNameString.split("/");
    QString lastPart;
    if (!fileNameParts.isEmpty()) {
        lastPart = fileNameParts.last();
    }
    return lastPart; 
}


void ScriptEngine::setIsAvatar(bool isAvatar) {
    _isAvatar = isAvatar;

    if (_isAvatar && !_avatarIdentityTimer) {
        // set up the avatar timers
        _avatarIdentityTimer = new QTimer(this);
        _avatarBillboardTimer = new QTimer(this);

        // connect our slot
        connect(_avatarIdentityTimer, &QTimer::timeout, this, &ScriptEngine::sendAvatarIdentityPacket);
        connect(_avatarBillboardTimer, &QTimer::timeout, this, &ScriptEngine::sendAvatarBillboardPacket);

        // start the timers
        _avatarIdentityTimer->start(AVATAR_IDENTITY_PACKET_SEND_INTERVAL_MSECS);
        _avatarBillboardTimer->start(AVATAR_BILLBOARD_PACKET_SEND_INTERVAL_MSECS);
    }

    if (!_isAvatar) {
        delete _avatarIdentityTimer;
        _avatarIdentityTimer = NULL;
        delete _avatarBillboardTimer;
        _avatarBillboardTimer = NULL;
    }
}

void ScriptEngine::setAvatarData(AvatarData* avatarData, const QString& objectName) {
    _avatarData = avatarData;

    // remove the old Avatar property, if it exists
    globalObject().setProperty(objectName, QScriptValue());

    // give the script engine the new Avatar script property
    registerGlobalObject(objectName, _avatarData);
}

void ScriptEngine::setAvatarHashMap(AvatarHashMap* avatarHashMap, const QString& objectName) {
    // remove the old Avatar property, if it exists
    globalObject().setProperty(objectName, QScriptValue());

    // give the script engine the new avatar hash map
    registerGlobalObject(objectName, avatarHashMap);
}

bool ScriptEngine::setScriptContents(const QString& scriptContents, const QString& fileNameString) {
    if (_isRunning) {
        return false;
    }
    _scriptContents = scriptContents;
    _fileNameString = fileNameString;
    return true;
}

void ScriptEngine::loadURL(const QUrl& scriptURL) {
    if (_isRunning) {
        return;
    }

    _fileNameString = scriptURL.toString();
    
    QUrl url(scriptURL);
    
    // if the scheme length is one or lower, maybe they typed in a file, let's try
    const int WINDOWS_DRIVE_LETTER_SIZE = 1;
    if (url.scheme().size() <= WINDOWS_DRIVE_LETTER_SIZE) {
        url = QUrl::fromLocalFile(_fileNameString);
    }
    
    // ok, let's see if it's valid... and if so, load it
    if (url.isValid()) {
        if (url.scheme() == "file") {
            _fileNameString = url.toLocalFile();
            QFile scriptFile(_fileNameString);
            if (scriptFile.open(QFile::ReadOnly | QFile::Text)) {
                qDebug() << "ScriptEngine loading file:" << _fileNameString;
                QTextStream in(&scriptFile);
                _scriptContents = in.readAll();
                emit scriptLoaded(_fileNameString);
            } else {
                qDebug() << "ERROR Loading file:" << _fileNameString;
                emit errorLoadingScript(_fileNameString);
            }
        } else {
            bool isPending;
            auto scriptCache = DependencyManager::get<ScriptCache>();
            scriptCache->getScript(url, this, isPending);
            
        }
    }
}

void ScriptEngine::scriptContentsAvailable(const QUrl& url, const QString& scriptContents) {
    _scriptContents = scriptContents;
    emit scriptLoaded(_fileNameString);
}

void ScriptEngine::errorInLoadingScript(const QUrl& url) {
    qDebug() << "ERROR Loading file:" << url.toString();
    emit errorLoadingScript(_fileNameString); // ??
}

void ScriptEngine::init() {
    if (_isInitialized) {
        return; // only initialize once
    }
    
    _isInitialized = true;

    auto sceneScriptingInterface = DependencyManager::set<SceneScriptingInterface>();

    auto entityScriptingInterface = DependencyManager::get<EntityScriptingInterface>();
    entityScriptingInterface->init();

    // register various meta-types
    registerMetaTypes(this);
    registerMIDIMetaTypes(this);
    registerEventTypes(this);
    registerMenuItemProperties(this);
    registerAnimationTypes(this);
    registerAvatarTypes(this);
    registerAudioMetaTypes(this);

    qScriptRegisterMetaType(this, EntityItemPropertiesToScriptValue, EntityItemPropertiesFromScriptValue);
    qScriptRegisterMetaType(this, EntityItemIDtoScriptValue, EntityItemIDfromScriptValue);
    qScriptRegisterMetaType(this, RayToEntityIntersectionResultToScriptValue, RayToEntityIntersectionResultFromScriptValue);
    qScriptRegisterSequenceMetaType<QVector<EntityItemID> >(this);

    qScriptRegisterSequenceMetaType<QVector<glm::vec2> >(this);
    qScriptRegisterSequenceMetaType<QVector<glm::quat> >(this);
    qScriptRegisterSequenceMetaType<QVector<QString> >(this);

    QScriptValue xmlHttpRequestConstructorValue = newFunction(XMLHttpRequestClass::constructor);
    globalObject().setProperty("XMLHttpRequest", xmlHttpRequestConstructorValue);

    QScriptValue printConstructorValue = newFunction(debugPrint);
    globalObject().setProperty("print", printConstructorValue);

    QScriptValue audioEffectOptionsConstructorValue = newFunction(AudioEffectOptions::constructor);
    globalObject().setProperty("AudioEffectOptions", audioEffectOptionsConstructorValue);
    
    qScriptRegisterMetaType(this, injectorToScriptValue, injectorFromScriptValue);
    qScriptRegisterMetaType(this, inputControllerToScriptValue, inputControllerFromScriptValue);
    qScriptRegisterMetaType(this, avatarDataToScriptValue, avatarDataFromScriptValue);
    qScriptRegisterMetaType(this, animationDetailsToScriptValue, animationDetailsFromScriptValue);

    registerGlobalObject("Script", this);
    registerGlobalObject("Audio", &AudioScriptingInterface::getInstance());
    registerGlobalObject("Controller", _controllerScriptingInterface);
    registerGlobalObject("Entities", entityScriptingInterface.data());
    registerGlobalObject("Quat", &_quatLibrary);
    registerGlobalObject("Vec3", &_vec3Library);
    registerGlobalObject("Uuid", &_uuidLibrary);
    registerGlobalObject("AnimationCache", DependencyManager::get<AnimationCache>().data());
    registerGlobalObject("Scene", DependencyManager::get<SceneScriptingInterface>().data());

    // constants
    globalObject().setProperty("TREE_SCALE", newVariant(QVariant(TREE_SCALE)));
    globalObject().setProperty("COLLISION_GROUP_ENVIRONMENT", newVariant(QVariant(COLLISION_GROUP_ENVIRONMENT)));
    globalObject().setProperty("COLLISION_GROUP_AVATARS", newVariant(QVariant(COLLISION_GROUP_AVATARS)));
}

QScriptValue ScriptEngine::registerGlobalObject(const QString& name, QObject* object) {
    if (object) {
        QScriptValue value = newQObject(object);
        globalObject().setProperty(name, value);
        return value;
    }
    return QScriptValue::NullValue;
}

void ScriptEngine::registerFunction(const QString& name, QScriptEngine::FunctionSignature fun, int numArguments) {
    registerFunction(globalObject(), name, fun, numArguments);
}

void ScriptEngine::registerFunction(QScriptValue parent, const QString& name, QScriptEngine::FunctionSignature fun, int numArguments) {
    QScriptValue scriptFun = newFunction(fun, numArguments);
    parent.setProperty(name, scriptFun);
}

void ScriptEngine::registerGetterSetter(const QString& name, QScriptEngine::FunctionSignature getter,
                                        QScriptEngine::FunctionSignature setter, QScriptValue object) {
    QScriptValue setterFunction = newFunction(setter, 1);
    QScriptValue getterFunction = newFunction(getter);

    if (!object.isNull()) {
        object.setProperty(name, setterFunction, QScriptValue::PropertySetter);
        object.setProperty(name, getterFunction, QScriptValue::PropertyGetter);
    } else {
        globalObject().setProperty(name, setterFunction, QScriptValue::PropertySetter);
        globalObject().setProperty(name, getterFunction, QScriptValue::PropertyGetter);
    }
}

void ScriptEngine::evaluate() {
    if (_stoppingAllScripts) {
        return; // bail early
    }

    if (!_isInitialized) {
        init();
    }

    QScriptValue result = evaluate(_scriptContents);

    // TODO: why do we check this twice? It seems like the call to clearExcpetions() in the lower level evaluate call
    // will cause this code to never actually run...
    if (hasUncaughtException()) {
        int line = uncaughtExceptionLineNumber();
        qDebug() << "Uncaught exception at (" << _fileNameString << ") line" << line << ":" << result.toString();
        emit errorMessage("Uncaught exception at (" + _fileNameString + ") line" + QString::number(line) + ":" + result.toString());
        clearExceptions();
    }
}

QScriptValue ScriptEngine::evaluate(const QString& program, const QString& fileName, int lineNumber) {
    if (_stoppingAllScripts) {
        return QScriptValue(); // bail early
    }

    _evaluatesPending++;
    QScriptValue result = QScriptEngine::evaluate(program, fileName, lineNumber);
    if (hasUncaughtException()) {
        int line = uncaughtExceptionLineNumber();
        qDebug() << "Uncaught exception at (" << _fileNameString << " : " << fileName << ") line" << line << ": " << result.toString();
    }
    _evaluatesPending--;
    emit evaluationFinished(result, hasUncaughtException());
    clearExceptions();
    return result;
}

void ScriptEngine::sendAvatarIdentityPacket() {
    if (_isAvatar && _avatarData) {
        _avatarData->sendIdentityPacket();
    }
}

void ScriptEngine::sendAvatarBillboardPacket() {
    if (_isAvatar && _avatarData) {
        _avatarData->sendBillboardPacket();
    }
}

void ScriptEngine::run() {
    // TODO: can we add a short circuit for _stoppingAllScripts here? What does it mean to not start running if
    // we're in the process of stopping?

    if (!_isInitialized) {
        init();
    }
    _isRunning = true;
    _isFinished = false;
    emit runningStateChanged();

    QScriptValue result = evaluate(_scriptContents);

    QElapsedTimer startTime;
    startTime.start();

    int thisFrame = 0;

    auto nodeList = DependencyManager::get<NodeList>();
    auto entityScriptingInterface = DependencyManager::get<EntityScriptingInterface>();

    qint64 lastUpdate = usecTimestampNow();

    while (!_isFinished) {
        int usecToSleep = (thisFrame++ * SCRIPT_DATA_CALLBACK_USECS) - startTime.nsecsElapsed() / 1000; // nsec to usec
        if (usecToSleep > 0) {
            usleep(usecToSleep);
        }

        if (_isFinished) {
            break;
        }

        QCoreApplication::processEvents();

        if (_isFinished) {
            break;
        }

        if (!_isFinished && entityScriptingInterface->getEntityPacketSender()->serversExist()) {
            // release the queue of edit entity messages.
            entityScriptingInterface->getEntityPacketSender()->releaseQueuedMessages();

            // since we're in non-threaded mode, call process so that the packets are sent
            if (!entityScriptingInterface->getEntityPacketSender()->isThreaded()) {
                entityScriptingInterface->getEntityPacketSender()->process();
            }
        }

        if (!_isFinished && _isAvatar && _avatarData) {

            const int SCRIPT_AUDIO_BUFFER_SAMPLES = floor(((SCRIPT_DATA_CALLBACK_USECS * AudioConstants::SAMPLE_RATE)
                                                           / (1000 * 1000)) + 0.5);
            const int SCRIPT_AUDIO_BUFFER_BYTES = SCRIPT_AUDIO_BUFFER_SAMPLES * sizeof(int16_t);

            QByteArray avatarPacket = byteArrayWithPopulatedHeader(PacketTypeAvatarData);
            avatarPacket.append(_avatarData->toByteArray());

            nodeList->broadcastToNodes(avatarPacket, NodeSet() << NodeType::AvatarMixer);

            if (_isListeningToAudioStream || _avatarSound) {
                // if we have an avatar audio stream then send it out to our audio-mixer
                bool silentFrame = true;

                int16_t numAvailableSamples = SCRIPT_AUDIO_BUFFER_SAMPLES;
                const int16_t* nextSoundOutput = NULL;

                if (_avatarSound) {

                    const QByteArray& soundByteArray = _avatarSound->getByteArray();
                    nextSoundOutput = reinterpret_cast<const int16_t*>(soundByteArray.data()
                                                                       + _numAvatarSoundSentBytes);

                    int numAvailableBytes = (soundByteArray.size() - _numAvatarSoundSentBytes) > SCRIPT_AUDIO_BUFFER_BYTES
                        ? SCRIPT_AUDIO_BUFFER_BYTES
                        : soundByteArray.size() - _numAvatarSoundSentBytes;
                    numAvailableSamples = numAvailableBytes / sizeof(int16_t);


                    // check if the all of the _numAvatarAudioBufferSamples to be sent are silence
                    for (int i = 0; i < numAvailableSamples; ++i) {
                        if (nextSoundOutput[i] != 0) {
                            silentFrame = false;
                            break;
                        }
                    }

                    _numAvatarSoundSentBytes += numAvailableBytes;
                    if (_numAvatarSoundSentBytes == soundByteArray.size()) {
                        // we're done with this sound object - so set our pointer back to NULL
                        // and our sent bytes back to zero
                        _avatarSound = NULL;
                        _numAvatarSoundSentBytes = 0;
                    }
                }
                
                QByteArray audioPacket = byteArrayWithPopulatedHeader(silentFrame
                                                                      ? PacketTypeSilentAudioFrame
                                                                      : PacketTypeMicrophoneAudioNoEcho);

                QDataStream packetStream(&audioPacket, QIODevice::Append);

                // pack a placeholder value for sequence number for now, will be packed when destination node is known
                int numPreSequenceNumberBytes = audioPacket.size();
                packetStream << (quint16) 0;

                if (silentFrame) {
                    if (!_isListeningToAudioStream) {
                        // if we have a silent frame and we're not listening then just send nothing and break out of here
                        break;
                    }

                    // write the number of silent samples so the audio-mixer can uphold timing
                    packetStream.writeRawData(reinterpret_cast<const char*>(&SCRIPT_AUDIO_BUFFER_SAMPLES), sizeof(int16_t));

                    // use the orientation and position of this avatar for the source of this audio
                    packetStream.writeRawData(reinterpret_cast<const char*>(&_avatarData->getPosition()), sizeof(glm::vec3));
                    glm::quat headOrientation = _avatarData->getHeadOrientation();
                    packetStream.writeRawData(reinterpret_cast<const char*>(&headOrientation), sizeof(glm::quat));

                } else if (nextSoundOutput) {
                    // assume scripted avatar audio is mono and set channel flag to zero
                    packetStream << (quint8)0;

                    // use the orientation and position of this avatar for the source of this audio
                    packetStream.writeRawData(reinterpret_cast<const char*>(&_avatarData->getPosition()), sizeof(glm::vec3));
                    glm::quat headOrientation = _avatarData->getHeadOrientation();
                    packetStream.writeRawData(reinterpret_cast<const char*>(&headOrientation), sizeof(glm::quat));

                    // write the raw audio data
                    packetStream.writeRawData(reinterpret_cast<const char*>(nextSoundOutput), numAvailableSamples * sizeof(int16_t));
                }
                
                // write audio packet to AudioMixer nodes
                auto nodeList = DependencyManager::get<NodeList>();
                nodeList->eachNode([this, &nodeList, &audioPacket, &numPreSequenceNumberBytes](const SharedNodePointer& node){
                    // only send to nodes of type AudioMixer
                    if (node->getType() == NodeType::AudioMixer) {
                        // pack sequence number
                        quint16 sequence = _outgoingScriptAudioSequenceNumbers[node->getUUID()]++;
                        memcpy(audioPacket.data() + numPreSequenceNumberBytes, &sequence, sizeof(quint16));
                        
                        // send audio packet
                        nodeList->writeDatagram(audioPacket, node);
                    }
                });
            }
        }

        qint64 now = usecTimestampNow();
        float deltaTime = (float) (now - lastUpdate) / (float) USECS_PER_SECOND;

        if (hasUncaughtException()) {
            int line = uncaughtExceptionLineNumber();
            qDebug() << "Uncaught exception at (" << _fileNameString << ") line" << line << ":" << uncaughtException().toString();
            emit errorMessage("Uncaught exception at (" + _fileNameString + ") line" + QString::number(line) + ":" + uncaughtException().toString());
            clearExceptions();
        }

        if (!_isFinished) {
            emit update(deltaTime);
        }
        lastUpdate = now;
        
    }

    stopAllTimers(); // make sure all our timers are stopped if the script is ending
    emit scriptEnding();

    // kill the avatar identity timer
    delete _avatarIdentityTimer;

    if (entityScriptingInterface->getEntityPacketSender()->serversExist()) {
        // release the queue of edit entity messages.
        entityScriptingInterface->getEntityPacketSender()->releaseQueuedMessages();

        // since we're in non-threaded mode, call process so that the packets are sent
        if (!entityScriptingInterface->getEntityPacketSender()->isThreaded()) {
            entityScriptingInterface->getEntityPacketSender()->process();
        }
    }

    // If we were on a thread, then wait till it's done
    if (thread()) {
        thread()->quit();
    }

    emit finished(_fileNameString);

    _isRunning = false;
    emit runningStateChanged();

    emit doneRunning();

    _doneRunningThisScript = true;
}

// NOTE: This is private because it must be called on the same thread that created the timers, which is why
// we want to only call it in our own run "shutdown" processing.
void ScriptEngine::stopAllTimers() {
    QMutableHashIterator<QTimer*, QScriptValue> i(_timerFunctionMap);
    while (i.hasNext()) {
        i.next();
        QTimer* timer = i.key();
        stopTimer(timer);
    }
}

void ScriptEngine::stop() {
    _isFinished = true;
    emit runningStateChanged();
}

void ScriptEngine::timerFired() {
    QTimer* callingTimer = reinterpret_cast<QTimer*>(sender());
    QScriptValue timerFunction = _timerFunctionMap.value(callingTimer);
    
    if (!callingTimer->isActive()) {
        // this timer is done, we can kill it
        _timerFunctionMap.remove(callingTimer);
        delete callingTimer;
    }
    
    // call the associated JS function, if it exists
    if (timerFunction.isValid()) {
        timerFunction.call();
    }
}

QObject* ScriptEngine::setupTimerWithInterval(const QScriptValue& function, int intervalMS, bool isSingleShot) {
    // create the timer, add it to the map, and start it
    QTimer* newTimer = new QTimer(this);
    newTimer->setSingleShot(isSingleShot);

    connect(newTimer, &QTimer::timeout, this, &ScriptEngine::timerFired);

    // make sure the timer stops when the script does
    connect(this, &ScriptEngine::scriptEnding, newTimer, &QTimer::stop);

    _timerFunctionMap.insert(newTimer, function);

    newTimer->start(intervalMS);
    return newTimer;
}

QObject* ScriptEngine::setInterval(const QScriptValue& function, int intervalMS) {
    if (_stoppingAllScripts) {
        qDebug() << "Script.setInterval() while shutting down is ignored... parent script:" << getFilename();
        return NULL; // bail early
    }

    return setupTimerWithInterval(function, intervalMS, false);
}

QObject* ScriptEngine::setTimeout(const QScriptValue& function, int timeoutMS) {
    if (_stoppingAllScripts) {
        qDebug() << "Script.setTimeout() while shutting down is ignored... parent script:" << getFilename();
        return NULL; // bail early
    }

    return setupTimerWithInterval(function, timeoutMS, true);
}

void ScriptEngine::stopTimer(QTimer *timer) {
    if (_timerFunctionMap.contains(timer)) {
        timer->stop();
        _timerFunctionMap.remove(timer);
        delete timer;
    }
}

QUrl ScriptEngine::resolvePath(const QString& include) const {
    QUrl url(include);
    // first lets check to see if it's already a full URL
    if (!url.scheme().isEmpty()) {
        return url;
    }

    // we apparently weren't a fully qualified url, so, let's assume we're relative
    // to the original URL of our script
    QUrl parentURL;
    if (_parentURL.isEmpty()) {
        parentURL = QUrl(_fileNameString);
    } else {
        parentURL = QUrl(_parentURL);
    }
    // if the parent URL's scheme is empty, then this is probably a local file...
    if (parentURL.scheme().isEmpty()) {
        parentURL = QUrl::fromLocalFile(_fileNameString);
    }

    // at this point we should have a legitimate fully qualified URL for our parent
    url = parentURL.resolved(url);
    return url;
}

void ScriptEngine::print(const QString& message) {
    emit printedMessage(message);
}

// If a callback is specified, the included files will be loaded asynchronously and the callback will be called
// when all of the files have finished loading.
// If no callback is specified, the included files will be loaded synchronously and will block execution until
// all of the files have finished loading.
void ScriptEngine::include(const QStringList& includeFiles, QScriptValue callback) {
    if (_stoppingAllScripts) {
        qDebug() << "Script.include() while shutting down is ignored..."
                 << "includeFiles:" << includeFiles << "parent script:" << getFilename();
        return; // bail early
    }
    QList<QUrl> urls;
    for (QString file : includeFiles) {
        urls.append(resolvePath(file));
    }

    BatchLoader* loader = new BatchLoader(urls);
    
    auto evaluateScripts = [=](const QMap<QUrl, QString>& data) {
        for (QUrl url : urls) {
            QString contents = data[url];
            if (contents.isNull()) {
                qDebug() << "Error loading file: " << url;
            } else {
                QScriptValue result = evaluate(contents, url.toString());
            }
        }

        if (callback.isFunction()) {
            QScriptValue(callback).call();
        }

        loader->deleteLater();
    };

    connect(loader, &BatchLoader::finished, this, evaluateScripts);

    // If we are destroyed before the loader completes, make sure to clean it up
    connect(this, &QObject::destroyed, loader, &QObject::deleteLater);

    loader->start();

    if (!callback.isFunction() && !loader->isFinished()) {
        QEventLoop loop;
        QObject::connect(loader, &BatchLoader::finished, &loop, &QEventLoop::quit);
        loop.exec();
    }
}

void ScriptEngine::include(const QString& includeFile, QScriptValue callback) {
    if (_stoppingAllScripts) {
        qDebug() << "Script.include() while shutting down is ignored... " 
                 << "includeFile:" << includeFile << "parent script:" << getFilename();
        return; // bail early
    }

    QStringList urls;
    urls.append(includeFile);
    include(urls, callback);
}

// NOTE: The load() command is similar to the include() command except that it loads the script
// as a stand-alone script. To accomplish this, the ScriptEngine class just emits a signal which
// the Application or other context will connect to in order to know to actually load the script
void ScriptEngine::load(const QString& loadFile) {
    if (_stoppingAllScripts) {
        qDebug() << "Script.load() while shutting down is ignored... "
                 << "loadFile:" << loadFile << "parent script:" << getFilename();
        return; // bail early
    }

    QUrl url = resolvePath(loadFile);
    emit loadScript(url.toString(), false);
}

void ScriptEngine::nodeKilled(SharedNodePointer node) {
    _outgoingScriptAudioSequenceNumbers.remove(node->getUUID());
}
