// orderbook_v0.2.cpp
// C++20 version
// Compile with: g++ -std=c++20 -pthread -O2 -DORDERBOOK_SINGLE_MAIN orderbook_v0.2.cpp

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <format> // for std::format in C++20

// ----- type definitions -----
using Price = int32_t;
using Quantity = uint32_t;
using OrderId = uint64_t;
using TimePoint = std::chrono::system_clock::time_point;

// ----- enums -----
enum class Side { Buy, Sell };

enum class OrderType {
    GoodTillCancel,  // standard limit order
    FillAndKill,     // IOC: fill whatever possible, cancel remainder
    FillOrKill,      // FOK: must fully fill immediately or cancel
    GoodForDay,      // canceled at session end
    Market           // filled for the quantity, independent of price
};

// ----- Order -----
struct Order {
    Order(OrderType t, OrderId id, Side s, Price p, Quantity q,
          TimePoint ts = std::chrono::system_clock::now())
        : type(t), id(id), side(s), price(p),
          initialQuantity(q), remainingQuantity(q), timestamp(ts) {}

    OrderId GetOrderId() const { return id; }
    Side GetSide() const { return side; }
    OrderType GetOrderType() const { return type; }
    Price GetPrice() const { return price; }
    Quantity GetInitialQuantity() const { return initialQuantity; }
    Quantity GetRemainingQuantity() const { return remainingQuantity; }
    bool IsFilled() const { return remainingQuantity == 0; }

    // when a trade happens, fill quantity
    void Fill(Quantity q) {
        if (q > remainingQuantity) throw std::logic_error(std::format("Order {} fill > remaining", id));
        remainingQuantity -= q;
    }

    // convert a market order into a price-capped limit order
    void ToGoodTillCancel(Price worstPrice) {
        if (type == OrderType::Market) {
            type = OrderType::GoodTillCancel;
            price = worstPrice;
        }
    }

private:
    OrderType type;
    OrderId id;
    Side side;
    Price price;
    Quantity initialQuantity;
    Quantity remainingQuantity;
    TimePoint timestamp;
};

// ----- OrderModify -----
struct OrderModify {
    OrderModify(OrderId id, Side s, Price p, Quantity q)
        : orderId(id), side(s), price(p), quantity(q) {}
    OrderId GetOrderId() const { return orderId; }
    Price GetPrice() const { return price; }
    Side GetSide() const { return side; }
    Quantity GetQuantity() const { return quantity; }

    // produce a new Order preserving the type
    std::shared_ptr<Order> ToOrderPointer(OrderType type) const {
        return std::make_shared<Order>(type, orderId, side, price, quantity);
    }

private:
    OrderId orderId;
    Side side;
    Price price;
    Quantity quantity;
};

// ----- Trade -----
struct TradeInfo { OrderId orderId; Price price; Quantity quantity; };
struct Trade { Trade(TradeInfo b, TradeInfo a) : bid(b), ask(a) {} TradeInfo bid; TradeInfo ask; };
struct LevelData { uint32_t count = 0; uint64_t quantity = 0; };

// ----- OrderBook -----
class OrderBook {
public:
    using OrderPtr = std::shared_ptr<Order>;
    using OrderPointers = std::list<OrderPtr>;
    struct OrderEntry { OrderPtr order; OrderPointers::iterator it; };

    // constructor starts pruning thread
    OrderBook() : shutdown_(false), pruneThread_([this]{ PruneGoodForDayOrders(); }) {}

    // destructor joins thread
    ~OrderBook() {
        shutdown_.store(true);
        cv_.notify_one();
        if (pruneThread_.joinable()) pruneThread_.join();
    }

    // add order, return trades executed by this add
    std::vector<Trade> AddOrder(const OrderPtr& order) {
        std::scoped_lock lock(mutex_);
        if (orders_.contains(order->GetOrderId())) return {}; // duplicate id ignored

        // Market order conversion: convert into worst-price limit order
        if (order->GetOrderType() == OrderType::Market) {
            if (order->GetSide() == Side::Buy && !asks_.empty()) order->ToGoodTillCancel(asks_.rbegin()->first);
            else if (order->GetSide() == Side::Sell && !bids_.empty()) order->ToGoodTillCancel(bids_.rbegin()->first);
            else return {}; // no liquidity
        }

        // FillAndKill / FillOrKill pre-checks
        if (order->GetOrderType() == OrderType::FillAndKill &&
            !CanMatch(order->GetSide(), order->GetPrice())) return {};
        if (order->GetOrderType() == OrderType::FillOrKill &&
            !CanFullyFill(order->GetSide(), order->GetPrice(), order->GetInitialQuantity())) return {};

        // insert order
        OrderPointers::iterator it;
        if (order->GetSide() == Side::Buy) {
            auto& lvl = bids_[order->GetPrice()];
            lvl.push_back(order);
            it = std::prev(lvl.end());
        } else {
            auto& lvl = asks_[order->GetPrice()];
            lvl.push_back(order);
            it = std::prev(lvl.end());
        }

        orders_.insert({order->GetOrderId(), OrderEntry{order, it}});
        OnOrderAdded(order);

        return MatchOrders();
    }

    // cancel an order
    void CancelOrder(OrderId id) {
        std::scoped_lock lock(mutex_);
        CancelOrderInternal(id);
    }

    // cancel then re-add with same type
    std::vector<Trade> ModifyOrder(const OrderModify& mod) {
        OrderType typeToKeep;
        { std::scoped_lock lock(mutex_); if(!orders_.contains(mod.GetOrderId())) return {}; typeToKeep = orders_.at(mod.GetOrderId()).order->GetOrderType(); }
        CancelOrder(mod.GetOrderId());
        return AddOrder(mod.ToOrderPointer(typeToKeep));
    }

    // snapshot of top N levels
    std::vector<std::pair<Price,uint64_t>> GetBidLevels(size_t depth=5) const {
        std::scoped_lock lock(mutex_);
        std::vector<std::pair<Price,uint64_t>> out; out.reserve(depth);
        for (const auto& [p,lvl]: bids_) {
            if(out.size()>=depth) break;
            uint64_t qty=0; for(auto& o:lvl) qty+=o->GetRemainingQuantity();
            out.emplace_back(p,qty);
        }
        return out;
    }

    std::vector<std::pair<Price,uint64_t>> GetAskLevels(size_t depth=5) const {
        std::scoped_lock lock(mutex_);
        std::vector<std::pair<Price,uint64_t>> out; out.reserve(depth);
        for (const auto& [p,lvl]: asks_) {
            if(out.size()>=depth) break;
            uint64_t qty=0; for(auto& o:lvl) qty+=o->GetRemainingQuantity();
            out.emplace_back(p,qty);
        }
        return out;
    }

    size_t Size() const { std::scoped_lock lock(mutex_); return orders_.size(); }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> shutdown_;
    std::thread pruneThread_; // background pruning thread

    std::map<Price,OrderPointers,std::greater<Price>> bids_;
    std::map<Price,OrderPointers,std::less<Price>> asks_;
    std::unordered_map<OrderId,OrderEntry> orders_;
    std::map<Price,LevelData,std::greater<Price>> data_;

    // bookkeeping hooks
    void OnOrderAdded(const OrderPtr& order){ auto &ld=data_[order->GetPrice()]; ld.count++; ld.quantity+=order->GetInitialQuantity(); }
    void OnOrderCancelled(const OrderPtr& order){ auto it=data_.find(order->GetPrice()); if(it!=data_.end()){ it->second.count--; it->second.quantity-=order->GetRemainingQuantity(); if(it->second.count==0) data_.erase(it); } }
    void OnOrderMatched(Price price, Quantity qty, bool full){ auto it=data_.find(price); if(it==data_.end()) return; if(full) it->second.count--; it->second.quantity-=qty; if(it->second.count==0) data_.erase(it); }

    // matching helpers
    bool CanMatch(Side side, Price price) const {
        if(side==Side::Buy){ if(asks_.empty()) return false; return price>=asks_.begin()->first; }
        else{ if(bids_.empty()) return false; return price<=bids_.begin()->first; }
    }

    bool CanFullyFill(Side side, Price price, Quantity qty) const {
        if(!CanMatch(side,price)) return false;
        uint64_t remaining=qty;
        if(side==Side::Buy){
            for(auto& [p,lvl]: asks_){
                if(p>price) break;
                auto dit=data_.find(p);
                uint64_t avail=(dit!=data_.end())?dit->second.quantity:0;
                if(avail>=remaining) return true;
                if(avail>0) remaining-=std::min<uint64_t>(avail,remaining);
                if(remaining==0) return true;
            }
            return false;
        } else {
            for(auto& [p,lvl]: bids_){
                if(p<price) break;
                auto dit=data_.find(p);
                uint64_t avail=(dit!=data_.end())?dit->second.quantity:0;
                if(avail>=remaining) return true;
                if(avail>0) remaining-=std::min<uint64_t>(avail,remaining);
                if(remaining==0) return true;
            }
            return false;
        }
    }

    std::vector<Trade> MatchOrders() {
        std::vector<Trade> trades; trades.reserve(orders_.size());
        while(!bids_.empty() && !asks_.empty()){
            auto &[bidPrice,bids]=*bids_.begin(); auto &[askPrice,asks]=*asks_.begin();
            if(bidPrice<askPrice) break;
            while(!bids.empty() && !asks.empty()){
                auto bid=bids.front(), ask=asks.front();
                Quantity qty=std::min(bid->GetRemainingQuantity(),ask->GetRemainingQuantity());
                bid->Fill(qty); ask->Fill(qty);
                trades.emplace_back(Trade{TradeInfo{bid->GetOrderId(),askPrice,qty},TradeInfo{ask->GetOrderId(),askPrice,qty}});
                OnOrderMatched(bidPrice,qty,bid->IsFilled()); OnOrderMatched(askPrice,qty,ask->IsFilled());
                if(bid->IsFilled()){ orders_.erase(bid->GetOrderId()); bids.pop_front(); }
                if(ask->IsFilled()){ orders_.erase(ask->GetOrderId()); asks.pop_front(); }
            }
            if(bids.empty()){ bids_.erase(bidPrice); data_.erase(bidPrice); }
            if(asks.empty()){ asks_.erase(askPrice); data_.erase(askPrice); }
        }
        if(!bids_.empty() && !bids_.begin()->second.empty() && bids_.begin()->second.front()->GetOrderType()==OrderType::FillAndKill)
            CancelOrderInternal(bids_.begin()->second.front()->GetOrderId());
        if(!asks_.empty() && !asks_.begin()->second.empty() && asks_.begin()->second.front()->GetOrderType()==OrderType::FillAndKill)
            CancelOrderInternal(asks_.begin()->second.front()->GetOrderId());
        return trades;
    }

    void CancelOrderInternal(OrderId id){
        if(!orders_.contains(id)) return;
        auto entry=orders_.at(id); auto order=entry.order; auto it=entry.it;
        orders_.erase(id);
        if(order->GetSide()==Side::Sell){ auto &c=asks_.at(order->GetPrice()); c.erase(it); if(c.empty()) asks_.erase(order->GetPrice()); }
        else{ auto &c=bids_.at(order->GetPrice()); c.erase(it); if(c.empty()) bids_.erase(order->GetPrice()); }
        OnOrderCancelled(order);
    }

    void PruneGoodForDayOrders(){
        using namespace std::chrono; const int pruneHour=16;
        while(!shutdown_.load()){
            auto now=system_clock::now(); std::time_t now_c=system_clock::to_time_t(now);
            std::tm local_tm{}; localtime_r(&now_c,&local_tm);
            local_tm.tm_hour=pruneHour; local_tm.tm_min=0; local_tm.tm_sec=0;
            auto next=system_clock::from_time_t(std::mktime(&local_tm));
            if(next<=now) next+=hours(24); auto waitDuration=next-now+milliseconds(100);
            std::unique_lock lk(mutex_);
            if(cv_.wait_for(lk,waitDuration,[this]{ return shutdown_.load(); })) return;
            std::vector<OrderId> toCancel; for(auto &[id,entry]: orders_) if(entry.order->GetOrderType()==OrderType::GoodForDay) toCancel.push_back(id);
            for(auto id:toCancel) CancelOrderInternal(id);
        }
    }
};

// ----- main -----
#ifdef ORDERBOOK_SINGLE_MAIN
int main(){
    OrderBook ob;
    auto o1=std::make_shared<Order>(OrderType::GoodTillCancel,1,Side::Buy,100,10);
    auto o2=std::make_shared<Order>(OrderType::GoodTillCancel,2,Side::Sell,99,5);
    auto trades=ob.AddOrder(o1); trades=ob.AddOrder(o2);
    for(auto &t: trades) std::cout<<std::format("Trade: bid={} ask={} px={} qty={}\n", t.bid.orderId,t.ask.orderId,t.bid.price,t.bid.quantity);
    std::cout<<"size: "<<ob.Size()<<"\n";
    return 0;
}
#endif
