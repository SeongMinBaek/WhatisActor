// ---------------------------------------------------------------------------
// ActorModel.cpp
//
// MMORPG 서버를 만들기 전에 액터 모델(Actor Model)만 최소한으로 구현해 본 예제.
//
// 액터 모델의 핵심 규칙은 딱 세 줄이다.
//   1. 상태(state)는 액터 하나가 독점한다. 밖에서 직접 건드리지 않는다.
//   2. 밖에서는 오직 "메시지(Job)"만 던진다. 던지는 쪽은 기다리지 않는다.
//   3. 한 액터의 잡은 절대 두 스레드에서 동시에 실행되지 않는다.
//
// 3번이 보장되면 액터 내부에서는 mutex 가 단 하나도 필요 없어진다.
// 이 파일에서 InvenActor / ZoneActor / QuestActor 의 멤버를 보면
// unordered_map 을 그냥 쓰고 있는데, 그게 이 모델의 전부이자 목적이다.
//
// 빌드: Visual Studio, C++14 이상 (별도 설정 없이 v143 기본값으로 컴파일됨)
// 이 파일은 UTF-8(BOM 포함)로 저장되어 있다. BOM 덕에 컴파일러가 주석을 제대로
// 읽고, 문자열 리터럴은 실행 문자 집합(CP949)으로 변환되므로 콘솔에도 잘 찍힌다.
// ---------------------------------------------------------------------------

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// 0. 로그 유틸 (여러 스레드가 동시에 찍으므로 줄 단위로 잠근다)
// ---------------------------------------------------------------------------

static std::mutex GLogLock;

#define COUT(x)                                              \
	do {                                                     \
		std::lock_guard<std::mutex> logLock__(GLogLock);     \
		std::cout << x << std::endl;                         \
	} while (false)

// 지금 잡을 실행 중인 워커 번호. 같은 액터가 매번 다른 스레드에서 실행된다는 걸
// 로그로 눈으로 확인하려고 둔 것. (-1 이면 메인 스레드)
static thread_local int LWorkerId = -1;

static std::string WorkerTag()
{
	std::ostringstream oss;
	if (LWorkerId < 0)
		oss << "[main    ]";
	else
		oss << "[worker" << LWorkerId << " ]";
	return oss.str();
}

// ---------------------------------------------------------------------------
// 1. Job - 액터에게 보내는 메시지 한 개
//
// "나중에 실행할 함수 호출"을 통째로 싸서 들고 다니는 객체다.
// 인자를 값으로 복사해서 캡처하는 게 핵심. 실행 시점이 미뤄지기 때문에
// 참조로 들고 있으면 이미 사라진 스택을 가리키게 된다.
// ---------------------------------------------------------------------------

class Job
{
public:
	explicit Job(std::function<void()> callback)
		: mCallback(std::move(callback))
	{
	}

	// 멤버 함수 포인터 + 인자를 받아서 호출을 통째로 캡처한다.
	// owner 를 shared_ptr 로 잡아두므로, 잡이 큐에 남아 있는 동안
	// 대상 액터가 먼저 소멸하는 일은 없다.
	template<typename T, typename Ret, typename... Params, typename... Args>
	Job(std::shared_ptr<T> owner, Ret(T::* func)(Params...), Args... args)
	{
		mCallback = [owner, func, args...]()
			{
				(owner.get()->*func)(args...);
			};
	}

	void Execute()
	{
		mCallback();
	}

private:
	std::function<void()> mCallback;
};

using JobRef = std::shared_ptr<Job>;

// ---------------------------------------------------------------------------
// 2. Scheduler - 실행할 액터를 워커 스레드에게 나눠주는 곳
//
// 스레드는 액터에 고정되지 않는다. "일이 생긴 액터"만 큐에 올라가고
// 놀고 있는 워커가 그걸 집어간다. 액터 1만 개에 스레드 8개로 충분한 이유.
// ---------------------------------------------------------------------------

class Actor;
using ActorRef = std::shared_ptr<Actor>;

class Scheduler
{
public:
	Scheduler() : mStop(false) {}

	void Schedule(ActorRef actor)
	{
		{
			std::lock_guard<std::mutex> lock(mLock);
			mReady.push(std::move(actor));
		}
		mCV.notify_one();
	}

	// 실행할 액터를 하나 꺼낸다. 없으면 대기.
	// 종료 요청 + 큐까지 비었을 때만 nullptr 을 돌려준다.
	ActorRef PopOrWait()
	{
		std::unique_lock<std::mutex> lock(mLock);
		mCV.wait(lock, [this]() { return mStop || !mReady.empty(); });

		if (mReady.empty())
			return nullptr;

		ActorRef actor = mReady.front();
		mReady.pop();
		return actor;
	}

	void Stop()
	{
		{
			std::lock_guard<std::mutex> lock(mLock);
			mStop = true;
		}
		mCV.notify_all();
	}

private:
	std::mutex mLock;
	std::condition_variable mCV;
	std::queue<ActorRef> mReady;
	bool mStop;
};

static Scheduler GScheduler;

// 아직 처리되지 않은 잡의 총 개수. 종료 시점(= 연쇄 메시지까지 전부 소진)을
// 알아내는 용도로만 쓴다.
static std::atomic<long long> GPendingJobs(0);

// ---------------------------------------------------------------------------
// 3. Actor - 잡 큐 + "동시에 한 스레드만 실행" 보장
//
// 여기가 이 파일에서 제일 중요한 부분이다.
//
//   Push()  : 큐에 넣고, 카운트를 0 -> 1 로 바꾼 스레드만 스케줄러에 등록한다.
//   Flush() : 등록된 액터를 워커가 집어서 잡을 쭉 처리한다.
//
// 왜 이걸로 상호배제가 되는가?
//   - 액터가 실행 큐에 올라가는 경로는 두 개뿐이다.
//       (a) mJobCount 가 0 -> 1 이 되는 Push
//       (b) Flush 끝에서 "아직 남았네" 하고 스스로 다시 올리는 경우
//   - (a)는 카운트가 0일 때만 성립한다. 카운트가 0이라는 건 처리할 잡이 없다는
//     뜻이고, Flush 중인 스레드는 마지막 fetch_sub 전까지 카운트를 0으로
//     떨어뜨리지 않는다. 즉 실행 중에는 (a)가 절대 성립하지 않는다.
//   - 그래서 어느 순간이든 이 액터를 Flush 하는 스레드는 최대 한 개다.
//     락 없이도 mItems / mPlayerHp 같은 내부 상태가 안전한 이유가 이것.
// ---------------------------------------------------------------------------

class Actor : public std::enable_shared_from_this<Actor>
{
public:
	explicit Actor(const char* name)
		: mName(name), mJobCount(0), mFlushing(0)
	{
	}

	virtual ~Actor() {}

	const std::string& Name() const { return mName; }

	void Push(JobRef job)
	{
		GPendingJobs.fetch_add(1);

		{
			std::lock_guard<std::mutex> lock(mLock);
			mJobs.push(std::move(job));
		}

		// 0 -> 1 로 바꾼 스레드만 실행 권한(=스케줄 권한)을 가져간다.
		// 이미 1 이상이면 다른 스레드가 처리 중이거나 예약해 둔 상태이므로
		// 그냥 큐에 넣어두고 빠지면 된다.
		if (mJobCount.fetch_add(1) == 0)
			GScheduler.Schedule(shared_from_this());
	}

	void Flush()
	{
		// 위 설명이 맞는지 실제로 감시한다. 이 값이 1을 넘으면 모델이 깨진 것.
		if (mFlushing.fetch_add(1) != 0)
			COUT("[BUG] " << mName << " 액터가 두 스레드에서 동시에 실행됨!");

		int executed = 0;
		while (executed < kMaxJobsPerFlush)
		{
			JobRef job;
			{
				std::lock_guard<std::mutex> lock(mLock);
				if (mJobs.empty())
					break;

				job = mJobs.front();
				mJobs.pop();
			}

			// 잡 실행은 반드시 락 밖에서. 잡 안에서 다른 액터에 Push 하는 게
			// 정상 동작이라, 락을 쥔 채로 실행하면 데드락 위험이 생긴다.
			job->Execute();
			++executed;
			GPendingJobs.fetch_sub(1);
		}

		mFlushing.fetch_sub(1);

		// 내가 처리한 개수보다 카운트가 크면, 처리하는 동안 새 잡이 들어온 것.
		// 이때는 내가 직접 다시 실행 큐에 올린다. (다른 스레드는 못 올린다)
		// kMaxJobsPerFlush 로 끊고 나온 경우도 여기서 이어서 처리된다.
		// - 한 액터가 워커 하나를 영원히 붙잡는 기아 상태를 막는 장치.
		if (mJobCount.fetch_sub(executed) > executed)
			GScheduler.Schedule(shared_from_this());
	}

private:
	static const int kMaxJobsPerFlush = 64;

	std::string mName;

	std::mutex mLock;          // 잡 "큐" 자체를 보호할 뿐, 액터 상태와는 무관
	std::queue<JobRef> mJobs;
	std::atomic<int> mJobCount;
	std::atomic<int> mFlushing; // 상호배제 검증용
};

// 액터에게 비동기로 일을 시키는 헬퍼.
// 호출한 스레드는 큐에 넣기만 하고 즉시 돌아온다. 리턴값은 받을 수 없다.
template<typename T, typename Ret, typename... Params, typename... Args>
void DoAsync(std::shared_ptr<T> actor, Ret(T::* func)(Params...), Args... args)
{
	actor->Push(std::make_shared<Job>(actor, func, args...));
}

static void WorkerMain(int workerId)
{
	LWorkerId = workerId;

	while (true)
	{
		ActorRef actor = GScheduler.PopOrWait();
		if (actor == nullptr)
			break;

		actor->Flush();
	}
}

// ---------------------------------------------------------------------------
// 4. 게임 액터 3종
//
//   InvenActor : 플레이어별 아이템 소지 수량
//   ZoneActor  : 존 안의 플레이어 HP, 브로드캐스트
//   QuestActor : 퀘스트 진행도
//
// 흐름 (전부 비동기, 어디에도 락이 없다):
//
//   클라 스레드 -> Inven.OnUseItem
//                    아이템 있으면 차감 -> Zone.OnItemUsed
//                                            HP 회복 + 브로드캐스트 -> Quest.OnItemUsed
//                                                                       10회마다 -> Inven.OnGiveItem (보상)
// ---------------------------------------------------------------------------

static const int kItemPotion = 1001; // 사용할 아이템
static const int kItemReward = 2001; // 퀘스트 보상 아이템
static const int kQuestGoal = 10;    // 10번 쓸 때마다 퀘스트 1회 완료
static const int kPotionHeal = 30;
static const int kMaxHp = 1000;

static const int kLogPlayerId = 0; // 로그가 폭발하지 않게 0번 플레이어만 관찰

class InvenActor;
class ZoneActor;
class QuestActor;

static std::shared_ptr<InvenActor> GInven;
static std::shared_ptr<ZoneActor>  GZone;
static std::shared_ptr<QuestActor> GQuest;

struct InvenStats
{
	long long useSuccess;   // 실제로 아이템이 있어서 사용에 성공한 횟수
	long long useFailed;    // 아이템이 없어서 실패한 횟수
	long long remainPotion; // 남은 포션 총합
	long long rewardItem;   // 받은 보상 아이템 총합
};

struct ZoneStats
{
	long long broadcastCount;
	long long totalHeal;
};

struct QuestStats
{
	long long totalProgress;
	long long completedCount;
};

// --- 인벤 액터 -------------------------------------------------------------

class InvenActor : public Actor
{
public:
	InvenActor() : Actor("Inven"), mUseSuccess(0), mUseFailed(0) {}

	void OnGiveItem(int playerId, int itemId, int count);
	void OnUseItem(int playerId, int itemId);
	void Report(std::shared_ptr<std::promise<InvenStats>> out);

private:
	// mutex 없음. 이 액터의 잡은 동시에 하나만 돌기 때문에 필요가 없다.
	std::unordered_map<int, std::unordered_map<int, int>> mItems;
	long long mUseSuccess;
	long long mUseFailed;
};

// --- 존 액터 ---------------------------------------------------------------

class ZoneActor : public Actor
{
public:
	explicit ZoneActor(int zoneId)
		: Actor("Zone"), mZoneId(zoneId), mBroadcastCount(0), mTotalHeal(0), mLogPlayerUseCount(0)
	{
	}

	void OnEnter(int playerId);
	void OnItemUsed(int playerId, int itemId);
	void Report(std::shared_ptr<std::promise<ZoneStats>> out);

private:
	int mZoneId;
	std::unordered_map<int, int> mPlayerHp; // 역시 락 없음
	long long mBroadcastCount;
	long long mTotalHeal;
	long long mLogPlayerUseCount; // 로그 관찰용 플레이어의 사용 횟수
};

// --- 퀘스트 액터 -----------------------------------------------------------

class QuestActor : public Actor
{
public:
	QuestActor() : Actor("Quest"), mTotalProgress(0), mCompletedCount(0) {}

	void OnItemUsed(int playerId);
	void Report(std::shared_ptr<std::promise<QuestStats>> out);

private:
	std::unordered_map<int, int> mProgress;
	long long mTotalProgress;
	long long mCompletedCount;
};

// --- 구현 (상호 참조 때문에 클래스 선언을 다 끝내고 아래에 모아 뒀다) -------

void InvenActor::OnGiveItem(int playerId, int itemId, int count)
{
	mItems[playerId][itemId] += count;

	if (playerId == kLogPlayerId && itemId == kItemReward)
	{
		COUT(WorkerTag() << " Inven : player " << playerId
			<< " 퀘스트 보상 수령 (item " << itemId
			<< " x" << mItems[playerId][itemId] << ")");
	}
}

void InvenActor::OnUseItem(int playerId, int itemId)
{
	int& count = mItems[playerId][itemId];

	if (count <= 0)
	{
		// 소지품이 없다. 여기서 끝. 다른 액터를 깨우지 않는다.
		++mUseFailed;
		return;
	}

	--count;
	++mUseSuccess;

	// 다른 액터의 상태가 필요하면 "직접 읽지 말고" 메시지를 던진다.
	// GZone->mPlayerHp 를 직접 만지는 순간 이 모델은 무너진다.
	DoAsync(GZone, &ZoneActor::OnItemUsed, playerId, itemId);
}

void InvenActor::Report(std::shared_ptr<std::promise<InvenStats>> out)
{
	InvenStats stats;
	stats.useSuccess = mUseSuccess;
	stats.useFailed = mUseFailed;
	stats.remainPotion = 0;
	stats.rewardItem = 0;

	for (const auto& player : mItems)
	{
		for (const auto& item : player.second)
		{
			if (item.first == kItemPotion)
				stats.remainPotion += item.second;
			else if (item.first == kItemReward)
				stats.rewardItem += item.second;
		}
	}

	out->set_value(stats);
}

void ZoneActor::OnEnter(int playerId)
{
	mPlayerHp[playerId] = 500;
}

void ZoneActor::OnItemUsed(int playerId, int itemId)
{
	int& hp = mPlayerHp[playerId];

	int before = hp;
	hp = (hp + kPotionHeal > kMaxHp) ? kMaxHp : hp + kPotionHeal;
	mTotalHeal += (hp - before);

	// 실제 서버라면 여기서 주변 플레이어에게 패킷을 뿌린다.
	++mBroadcastCount;

	if (playerId == kLogPlayerId)
	{
		// 같은 액터가 매번 다른 워커에서 실행되는 걸 보여주려고 앞쪽 몇 개만 찍는다.
		++mLogPlayerUseCount;
		if (mLogPlayerUseCount <= 5)
		{
			COUT(WorkerTag() << " Zone  : player " << playerId
				<< " item " << itemId << " 사용, HP " << before << " -> " << hp);
		}
	}

	DoAsync(GQuest, &QuestActor::OnItemUsed, playerId);
}

void ZoneActor::Report(std::shared_ptr<std::promise<ZoneStats>> out)
{
	ZoneStats stats;
	stats.broadcastCount = mBroadcastCount;
	stats.totalHeal = mTotalHeal;
	out->set_value(stats);
}

void QuestActor::OnItemUsed(int playerId)
{
	int& progress = mProgress[playerId];

	++progress;
	++mTotalProgress;

	if (progress % kQuestGoal != 0)
		return;

	++mCompletedCount;

	if (playerId == kLogPlayerId)
	{
		COUT(WorkerTag() << " Quest : player " << playerId
			<< " 퀘스트 완료 (" << progress << "회 사용) -> 보상 지급 요청");
	}

	// 인벤으로 되돌아가는 메시지. 액터끼리 순환 호출이 되어도
	// 서로 블로킹하지 않기 때문에 데드락이 생기지 않는다.
	DoAsync(GInven, &InvenActor::OnGiveItem, playerId, kItemReward, 1);
}

void QuestActor::Report(std::shared_ptr<std::promise<QuestStats>> out)
{
	QuestStats stats;
	stats.totalProgress = mTotalProgress;
	stats.completedCount = mCompletedCount;
	out->set_value(stats);
}

// ---------------------------------------------------------------------------
// 5. 부하 생성 - IOCP 워커가 패킷을 받아서 액터에 밀어넣는 상황을 흉내
// ---------------------------------------------------------------------------

static const int kPlayerCount = 100;
static const int kInitPotion = 50;
static const int kClientThreads = 8;
static const int kRequestPerThread = 5000;

static void ClientThreadMain(int seed)
{
	std::mt19937 rng(static_cast<unsigned int>(seed));
	std::uniform_int_distribution<int> dist(0, kPlayerCount - 1);

	for (int i = 0; i < kRequestPerThread; ++i)
	{
		int playerId = dist(rng);

		// 여러 스레드가 같은 액터에 동시에 Push 한다.
		// 그래도 실행은 한 번에 하나씩만 일어난다.
		DoAsync(GInven, &InvenActor::OnUseItem, playerId, kItemPotion);
	}
}

// 연쇄 메시지까지 전부 소진될 때까지 기다린다.
static void WaitUntilDrained()
{
	while (GPendingJobs.load() > 0)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

// 액터의 상태를 밖에서 읽는 유일하게 올바른 방법: 잡을 넣고 결과를 받는다.
// (Ask 패턴. promise 는 복사가 안 되므로 shared_ptr 로 감싸서 넘긴다)
template<typename TActor, typename TStats>
static TStats Ask(std::shared_ptr<TActor> actor,
	void (TActor::* func)(std::shared_ptr<std::promise<TStats>>))
{
	auto promise = std::make_shared<std::promise<TStats>>();
	std::future<TStats> future = promise->get_future();

	DoAsync(actor, func, promise);

	return future.get();
}

int main()
{
	int workerCount = static_cast<int>(std::thread::hardware_concurrency());
	if (workerCount < 4)
		workerCount = 4;

	COUT("=== Actor Model 예제 ===");
	COUT("워커 스레드 : " << workerCount
		<< " / 클라 스레드 : " << kClientThreads
		<< " / 총 요청 : " << (kClientThreads * kRequestPerThread));
	COUT("");

	// --- 액터 생성. 반드시 shared_ptr 로 만들어야 한다.
	//     Push() 안에서 shared_from_this() 를 쓰기 때문.
	GInven = std::make_shared<InvenActor>();
	GZone = std::make_shared<ZoneActor>(1);
	GQuest = std::make_shared<QuestActor>();

	// --- 워커 스레드 기동
	std::vector<std::thread> workers;
	workers.reserve(workerCount);
	for (int i = 0; i < workerCount; ++i)
		workers.push_back(std::thread(WorkerMain, i));

	// --- 초기 세팅도 전부 메시지로. 여기서 mItems 를 직접 채우면 안 된다.
	for (int playerId = 0; playerId < kPlayerCount; ++playerId)
	{
		DoAsync(GZone, &ZoneActor::OnEnter, playerId);
		DoAsync(GInven, &InvenActor::OnGiveItem, playerId, kItemPotion, kInitPotion);
	}
	WaitUntilDrained();

	// --- 부하 시작
	std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();

	std::vector<std::thread> clients;
	clients.reserve(kClientThreads);
	for (int i = 0; i < kClientThreads; ++i)
		clients.push_back(std::thread(ClientThreadMain, i + 1));

	for (auto& t : clients)
		t.join();

	// 클라 스레드가 끝나도 잡은 아직 남아 있다. 연쇄 메시지까지 기다린다.
	WaitUntilDrained();

	std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
	long long elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count();

	// --- 결과 수집
	InvenStats inven = Ask<InvenActor, InvenStats>(GInven, &InvenActor::Report);
	ZoneStats  zone = Ask<ZoneActor, ZoneStats>(GZone, &ZoneActor::Report);
	QuestStats quest = Ask<QuestActor, QuestStats>(GQuest, &QuestActor::Report);

	WaitUntilDrained();

	// --- 워커 종료
	GScheduler.Stop();
	for (auto& t : workers)
		t.join();

	// --- 검증
	// 락을 하나도 안 걸었는데 아래 항등식이 전부 맞아야 한다.
	const long long totalRequest = static_cast<long long>(kClientThreads) * kRequestPerThread;
	const long long totalGranted = static_cast<long long>(kPlayerCount) * kInitPotion;

	COUT("");
	COUT("=== 결과 (" << elapsedMs << " ms) ===");
	COUT("요청 총합       : " << totalRequest
		<< "  (성공 " << inven.useSuccess << " + 실패 " << inven.useFailed << ")");
	COUT("포션 지급/잔여  : " << totalGranted << " / " << inven.remainPotion);
	COUT("존 브로드캐스트 : " << zone.broadcastCount << " (총 회복량 " << zone.totalHeal << ")");
	COUT("퀘스트 진행/완료: " << quest.totalProgress << " / " << quest.completedCount);
	COUT("보상 아이템     : " << inven.rewardItem);
	COUT("");

	bool ok = true;
	ok = ok && (inven.useSuccess + inven.useFailed == totalRequest);    // 요청이 하나도 유실되지 않았다
	ok = ok && (inven.useSuccess + inven.remainPotion == totalGranted); // 아이템이 복사되거나 증발하지 않았다
	ok = ok && (inven.useSuccess == zone.broadcastCount);               // 인벤 -> 존 메시지가 전부 도착했다
	ok = ok && (zone.broadcastCount == quest.totalProgress);            // 존 -> 퀘스트 메시지도 마찬가지
	ok = ok && (quest.completedCount == inven.rewardItem);              // 퀘스트 -> 인벤 보상도 정확히 일치

	COUT((ok ? "검증 통과 : 락 하나 없이 모든 항등식이 성립했다."
		: "검증 실패 : 어딘가 레이스가 있다."));

	COUT("");
	COUT("메모: 위 액터들의 unordered_map 에는 mutex 가 하나도 없다.");
	COUT("      같은 map 을 여러 스레드에서 그냥 건드렸다면 진작 터졌을 것이다.");

	GInven.reset();
	GZone.reset();
	GQuest.reset();

	return ok ? 0 : 1;
}
