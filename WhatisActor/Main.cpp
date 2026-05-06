#include <iostream>
#include <queue>
#include <thread>
#include <functional>
#include <unordered_map>

#define COUT(x) cout<<x<<endl;

using namespace std;

namespace EActor
{
	enum Type
	{
		None = 0,

		Zone,
		Inven,
		Quest,

		Max
	};
}

namespace EMessage
{
	enum Type
	{
		None = 0,

		// 100~200 존 타입
		ZoneType = 100,

		ZoneTypeMax = 199,

		// 200~300 인벤 타입
		InvenType = 200,
		InvenItemUse = 201,

		InvenTypeMax = 299,

		// 300~400 퀘스트 타입
		QuestType = 300,

		QuestTypeMax = 399,

		Max
	};

	inline bool IsZoneType(Type t)
	{
		if (ZoneType < t
			&& ZoneTypeMax < t)
			return true;
		return false;
	}

	inline bool IsInvenType(Type t)
	{
		if (InvenType < t
			&& InvenTypeMax < t)
			return true;
		return false;
	}

	inline bool IsQuestType(Type t)
	{
		if (QuestType < t
			&& QuestTypeMax < t)
			return true;
		return false;
	}
}

class IData
{
public:
	EMessage::Type mType;
	EActor::Type mFrom;

	IData(EMessage::Type type)
	{
		this->mType = type;
		this->mFrom = EActor::None;
	}

	void SetFrom(EActor::Type type)
	{
		this->mFrom = type;
	}

	template <typename T>
	T* TryParse()
	{
		return static_cast<T*>(this);
	}
};

class UseItem : public IData
{
public:
	int64_t userID;
	int itemID;

	UseItem(int64_t userID, int itemID) : IData(EMessage::InvenItemUse)
	{
		this->userID = userID;
		this->itemID = itemID;
	}
};

class MMessage : public std::enable_shared_from_this<MMessage>
{
protected:
	

public:
	std::shared_ptr<MMessage> mpNext;
	IData* mData;

	MMessage(IData* pData)
	{
		mpNext = nullptr;
		this->mData = pData;

		//COUT("MMessage Create!!");
	}

	~MMessage()
	{
		if (mData != nullptr)
			delete mData;

		//COUT("MMessage Delete!!");
	}

	static std::shared_ptr<MMessage> Create(IData* pData)
	{
		return std::make_shared<MMessage>(pData);
	}
};

template<typename T>
class Singleton
{
private:
	static T mInstance;

public:
	static T& Get()
	{
		return mInstance;
	}
};

template<typename T>
T Singleton<T>::mInstance;

class AtomicQueue
{
private:
	std::shared_ptr<MMessage> mTail;
	std::shared_ptr<MMessage> mHead;

public:
	AtomicQueue()
	{
		mHead = MMessage::Create(new IData(EMessage::None));
		Push(mHead);
	}

	bool Push(std::shared_ptr<MMessage> post)
	{
		post->mpNext = nullptr;
		std::shared_ptr<MMessage> prevTail = std::atomic_exchange(&mTail, post); //mTail.exchange(post);

		if (prevTail != nullptr)
			prevTail->mpNext = post;

		return true;
	}

	std::shared_ptr<MMessage> Pop()
	{
		if (mHead == nullptr)
			return nullptr;

		std::shared_ptr<MMessage> h = mHead->mpNext;

		if (h)
		{
			mHead->mpNext = nullptr;
			mHead = h;
			return h;
		}

		return nullptr;
	}
};

class Actor
{
protected:
	AtomicQueue mQueue;

	EActor::Type mType;

	std::thread mThread;
	std::unordered_map<int, std::function<bool(IData*)>> mFunc;

	bool mRunning;

public:
	Actor(EActor::Type type)
	{
		mType = type;	
		mRunning = true;
	}

	~Actor()
	{
		if (mThread.joinable())
			mThread.join();
	}

	void Push(std::shared_ptr<MMessage> msg)
	{
		mQueue.Push(msg);
	}

	virtual void Init() { }

	virtual void Start() 
	{
		Init();

		mRunning = true;
		mThread = std::thread(&Actor::DispatchMessage, this);
	}

	void Stop()
	{
		mRunning = false;
	}

	virtual void DispatchMessage()
	{
		while (true)
		{
			if (mRunning == false)
				break;

			auto msg = mQueue.Pop();
			if (msg == nullptr)
				continue;

			if(mFunc.find(msg->mData->mType) != mFunc.end())
				mFunc[msg->mData->mType](msg->mData);			
		}
	}

	void RegisterFunc(int msgID, std::function<bool(IData*)> func)
	{
		mFunc[msgID] = func;
	};
};

class InvenActor : public Singleton<InvenActor>, public Actor
{
private:

public:
	InvenActor() : Actor(EActor::Inven)
	{
	}

	virtual void Init() override
	{
		RegisterFunc(EMessage::InvenItemUse, std::bind(&InvenActor::OnUseItem, this, std::placeholders::_1));
	}

	bool OnUseItem(IData* data)
	{
		if (data->mType == EMessage::InvenItemUse)
		{
			UseItem* useItem = data->TryParse<UseItem>();
			if (useItem != nullptr)
				COUT("UserID : " << useItem->userID << " ItemID : " << useItem->itemID);
		}

		return true;
	}
};

class ZoneActor : public Singleton<InvenActor>, public Actor
{
private:

public:

};

static int sValue = 0;
int threadCount = 1000;
int limitCount = 1000;
std::atomic<int> threadID = 0;

void AddData()
{
	while (sValue < limitCount)
	{
		UseItem* useITem = new UseItem(100, ++sValue);
		useITem->mType = EMessage::InvenItemUse;

		InvenActor::Get().Push(MMessage::Create(useITem));
	}
	int threadid = threadID.load();
	threadID.exchange(threadid + 1);
	COUT("~Thread : " << threadid);
}

void Run()
{
	std::vector<std::thread> threads;
	threads.reserve(threadCount);

	for (int i = 0; i < threadCount; ++i)
	{
		threads.push_back(std::thread(AddData));
	}

	for (auto& t : threads)
	{
		if (t.joinable())
			t.join();
	}
}

int main()
{
	InvenActor::Get().Start();
	Run();

	/*InvenActor::Get().Start();
	InvenActor::Get().Push(MMessage::Create(new UseItem(100, 200)));
	InvenActor::Get().Push(MMessage::Create(new UseItem(100, 300)));
	InvenActor::Get().Push(MMessage::Create(new UseItem(100, 400)));*/
}