#include "boxedwine.h"

#include "kstat.h"

FsNode::FsNode(Type type, U32 id, U32 rdev, const std::string& path, const std::string& link, const std::string& nativePath, bool isDirectory, BoxedPtr<FsNode> parent) : 
    path(path),
    nativePath(nativePath),
    name(Fs::getFileNameFromPath(path)),    
    link(link),    
    id(id), 
    rdev(rdev),
    hardLinkCount(1),
    type(type),  
    parent(parent),
    isDir(isDirectory),  
    locksCS("FsNode.lockCS"),
    hasLoadedChildrenFromFileSystem(false)
 {   
}

void FsNode::removeOpenNode(FsOpenNode* node) {
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(this->openNodesMutex);
    node->listNode.remove();
}

bool FsNode::canRead() {
    return (getMode() & K__S_IREAD)!=0;
}

bool FsNode::canWrite() {
    return (getMode() & K__S_IWRITE)!=0;
}

void FsNode::removeNodeFromParent() {
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(this->parent->childrenByNameMutex);
    this->parent->childrenByName.erase(this->name);
}

void FsNode::loadChildren() {
    // don't need to protect from threads since this is private
    if (!this->hasLoadedChildrenFromFileSystem) {
        this->hasLoadedChildrenFromFileSystem = true;
        if (this->nativePath.length()) {
            std::vector<Platform::ListNodeResult> results;
            Platform::listNodes(nativePath, results);
            for (auto& n : results) {
                std::string localPath = this->path;
                std::string remotePath = this->nativePath+Fs::nativePathSeperator+n.name;
                if (!stringHasEnding(localPath, "/"))
                    localPath+="/";
                Fs::remoteNameToLocal(n.name);
                localPath+=n.name;
                if (stringHasEnding(localPath, ".mixed")) {
                    localPath = localPath.substr(0, localPath.length()-6);
                }
                if (!stringHasEnding(localPath, ".link")) {
                    Fs::addFileNode(localPath, "", remotePath, n.isDirectory, this);
                } else {
                    U8 tmp[MAX_FILEPATH_LEN];
                    U32 result = Fs::readNativeFile(remotePath, tmp, MAX_FILEPATH_LEN-1);
                    tmp[result]=0;
                    if (result==0) {
                        kwarn("Could not read link file from filesystem: %s", localPath.c_str());
                    }
                    localPath = localPath.substr(0, localPath.length()-5);
                    Fs::addFileNode(localPath, (const char*)tmp, remotePath, n.isDirectory, this);
                }           
            }
        }
    }
}

BoxedPtr<FsNode> FsNode::getChildByName(const std::string& name) {
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(this->childrenByNameMutex);
    this->loadChildren();
    if (this->childrenByName.count(name))
        return this->childrenByName[name];
    return NULL;
}

BoxedPtr<FsNode> FsNode::getChildByNameIgnoreCase(const std::string& name) {
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(this->childrenByNameMutex);
    this->loadChildren();
    for (auto& n : this->childrenByName) {
        if (stringCaseInSensativeEquals(n.first, name)) {
            return n.second;
        }
    }
    return NULL;
}

U32 FsNode::getChildCount() {
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(this->childrenByNameMutex);
    this->loadChildren();
    return (U32)this->childrenByName.size();
}

void FsNode::addChild(BoxedPtr<FsNode> node) {
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(this->childrenByNameMutex);
    this->loadChildren();
    this->childrenByName[node->name] = node;
}

void FsNode::removeChildByName(const std::string& name) {
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(this->childrenByNameMutex);
    this->loadChildren();
    this->childrenByName.erase(name);
}

void FsNode::getAllChildren(std::vector<BoxedPtr<FsNode> > & results) {
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(this->childrenByNameMutex);
    this->loadChildren();
    for (auto& n : this->childrenByName) {
        results.push_back(n.second);
    }
}

bool FsNode::unlock(KFileLock* lock) {
    BOXEDWINE_CRITICAL_SECTION_WITH_CONDITION(this->locksCS);
    KFileLock* found = getLock(lock, false);
    bool result = false;

    while (found) {
        this->locks.erase(std::remove(locks.begin(), locks.end(), *found), locks.end());
        found = getLock(lock, false);
        result = true;
    }
    if (result) {
        BOXEDWINE_CONDITION_SIGNAL(this->locksCS);
    }
    return result;
}

U32 FsNode::addLock(KFileLock* lock) {
    BOXEDWINE_CRITICAL_SECTION_WITH_CONDITION(this->locksCS);
    KFileLock* found = getLock(lock, true);
    if (found) {
        return -K_EAGAIN;
    }
    this->locks.push_back(*lock);    
    return 0;
}

U32 FsNode::addLockAndWait(KFileLock* lock, bool otherProcess) {
    BOXEDWINE_CRITICAL_SECTION_WITH_CONDITION(this->locksCS);
    KFileLock* found = getLock(lock, otherProcess);
    bool result = false;

    while (found) {
        BOXEDWINE_CONDITION_WAIT(this->locksCS);
        found = getLock(lock, otherProcess);
    }
    this->locks.push_back(*lock);
    return 0;
}

bool FsNode::hasLock(U32 pid) {
    BOXEDWINE_CRITICAL_SECTION_WITH_CONDITION(this->locksCS);

    for (auto& n : this->locks) {
        KFileLock* next = &n;
        if (next->l_pid == pid) {
            return true;
        }
    }
    return false;
}

void FsNode::unlockAll(U32 pid) {
    BOXEDWINE_CRITICAL_SECTION_WITH_CONDITION(this->locksCS);
    auto it = std::remove_if(locks.begin(), locks.end(), [pid](auto&& item)
        { return (item.l_pid == pid); });
    locks.erase(it, locks.end());
    BOXEDWINE_CONDITION_SIGNAL(this->locksCS);
}

KFileLock* FsNode::getLock(KFileLock* lock, bool otherProcess) {
    BOXEDWINE_CRITICAL_SECTION_WITH_CONDITION(this->locksCS);
    U64 l1 = lock->l_start;
    U64 l2 = l1+lock->l_len;
    U32 pid = KThread::currentThread()->process->id;

    if (lock->l_len == 0)
        l2 = 0xFFFFFFFF;
    for( auto& n : this->locks ) {
        KFileLock* next = &n;
        U64 s1 = next->l_start;
        U64 s2 = s1+next->l_len;
        
        if (next->l_len == 0)
            s2 = 0xFFFFFFFF;
        if (((s1>=l1 && s1<=l2) || (s2>=l1 && s2<=l2)) && ((otherProcess && next->l_pid != pid) || (!otherProcess && next->l_pid == pid))) {
            return next;
        }
    }
    return NULL;
 }

void FsNode::addOpenNode(KListNode<FsOpenNode*>* node) {
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(this->openNodesMutex);
    this->openNodes.addToBack(node);
}