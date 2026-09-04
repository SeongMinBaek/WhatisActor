#include <iostream>
#include <queue>
#include <thread>
#include <functional>
#include <unordered_map>

#define COUT(x) cout<<x<<endl;

using namespace std;

//template<typename T>
//class AtomicQueue;
//
//thread_local shared_ptr<AtomicQueue<T>> TLJQueue;
//
//namespace EActor
//{
//	enum Type
//	{
//		None = 0,
//
//		Zone,
//		Inven,
//		Quest,
//
//		Max
//	};
//}
//
//namespace EMessage
//{
//	enum Type
//	{
//		None = 0,
//
//		// 100~200 존 타입
//		ZoneType = 100,
//
//		ZoneTypeMax = 199,
//
//		// 200~300 인벤 타입
//		InvenType = 200,
//		InvenItemUse = 201,
//
//		InvenTypeMax = 299,
//
//		// 300~400 퀘스트 타입
//		QuestType = 300,
//
//		QuestTypeMax = 399,
//
//		Max
//	};
//
//	inline bool IsZoneType(Type t)
//	{
//		if (ZoneType < t
//			&& ZoneTypeMax < t)
//			return true;
//		return false;
//	}
//
//	inline bool IsInvenType(Type t)
//	{
//		if (InvenType < t
//			&& InvenTypeMax < t)
//			return true;
//		return false;
//	}
//
//	inline bool IsQuestType(Type t)
//	{
//		if (QuestType < t
//			&& QuestTypeMax < t)
//			return true;
//		return false;
//	}
//}
//
//class CSUseItem
//{
//public:
//	int itemID;
//
//	CSUseItem(int itemID)
//	{
//		this->itemID = itemID;
//	}
//};
//
//class Session
//{
//public:
//	int mSessionID;
//	int mUserID;
//
//	
//
//	Session()
//	{
//		mSessionID = 0;
//		mUserID = 0;
//	}
//
//	void OnRequest_ItemUse(CSUseItem msg)
//	{
//		COUT("SessionID : " << mSessionID << ", UserID : " << mUserID << ", ItemID : " << msg.itemID);
//	}
//};
//
//class Job : std::enable_shared_from_this<Job>
//{
//public:
//	shared_ptr<Job> mpNext;
//	std::function<void()> mExcute;
//
//	template<typename T, typename Ref, typename... Args>
//	Job(shared_ptr<T> session, Ref(T::*mFunc)(Args...), Args&&... args)
//	{
//		mExcute = [session, mFunc, args...]()
//			{
//				(session.get()->*mFunc)(args...);
//			};
//	};
//
//	template<typename T, typename Ref>
//	Job(shared_ptr<T> session, Ref(T::* mFunc)())
//	{
//		mExcute = [session, mFunc]()
//			{
//				(session.get()->*mFunc)();
//			};
//	};
//};
//
//template<typename T>
//class Singleton
//{
//private:
//	static T mInstance;
//
//public:
//	static T& Get()
//	{
//		return mInstance;
//	}
//};
//
//template<typename T>
//T Singleton<T>::mInstance;
//
//template<typename T>
//class AtomicQueue
//{
//private:
//	std::shared_ptr<T> mTail;
//	std::shared_ptr<T> mHead;
//
//	std::atomic<int> mCount;
//public:
//	AtomicQueue()
//	{
//		mHead = std::make_shared<T>();
//		Push(mHead);
//		mCount = 0;
//	}
//
//	bool Push(std::shared_ptr<T> post)
//	{
//		post->mpNext = nullptr;
//		std::shared_ptr<T> prevTail = std::atomic_exchange(&mTail, post); //mTail.exchange(post);
//		mCount.fetch_add(1);
//
//		if (prevTail != nullptr)
//			prevTail->mpNext = post;
//
//		while (Pop())
//		{
//			
//		}
//
//		return true;
//	}
//
//	std::shared_ptr<T> Pop()
//	{
//		if (mHead == nullptr)
//			return nullptr;
//
//		std::shared_ptr<T> h = mHead->mpNext;
//
//		if (h)
//		{
//			mCount.fetch_sub(1);
//			mHead->mpNext = nullptr;
//			mHead = h;
//			return h;
//		}
//
//		return nullptr;
//	}
//};
//
//class Actor
//{
//protected:
//	AtomicQueue<Job> mQueue;
//	EActor::Type mType;
//
//public:
//	Actor(EActor::Type type)
//	{
//		mType = type;	
//	}
//
//	void Push(std::shared_ptr<Job> msg)
//	{
//		mQueue.Push(msg);
//	}
//};
//
//class InvenActor : public Singleton<InvenActor>, public Actor
//{
//private:
//
//public:
//	InvenActor() : Actor(EActor::Inven)
//	{
//	}
//};
//
//static int sValue = 0;
//int threadCount = 1000;
//int limitCount = 1000;
//std::atomic<int> threadID = 0;
//
//void AddData()
//{
//	int threadid = threadID.load();
//	threadID.exchange(threadid + 1);
//
//	std::shared_ptr<Session> session = std::make_shared<Session>();
//	session->mSessionID = threadid;
//	session->mUserID = threadid;
//
//	while (sValue < limitCount)
//	{
//		CSUseItem useItem(++sValue);
//		InvenActor::Get().Push(std::make_shared<Job>(session, &Session::OnRequest_ItemUse, useItem));
//	}
//	
//	COUT("~Thread : " << threadid);
//}
//
//void Run()
//{
//	std::vector<std::thread> threads;
//	threads.reserve(threadCount);
//
//	for (int i = 0; i < threadCount; ++i)
//	{
//		threads.push_back(std::thread(AddData));
//	}
//
//	for (auto& t : threads)
//	{
//		if (t.joinable())
//			t.join();
//	}
//}

int main()
{
	//Run();

	std::unordered_map<int, int> tmap;
	tmap.reserve(171);

	for (int i = 0; i < 200; i++)
	{
		tmap.insert({i, i});
	}

	int size = 0;

	for (const auto& it : tmap)
	{
		if (it.second > 171)
			continue;

		++size;
	}

	int earnPoint = 0;
	float rate = static_cast<float>(10000000) / static_cast<float>(size);

	earnPoint = static_cast<int>(rate);

	COUT(size);
	COUT(earnPoint);
}