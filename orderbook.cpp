#include <iostream>
#include <map>
#include <set> 
#include <list>
#include <cmath>
#include <ctime>
#include <deque>
#include <queue> 
#include <stack>
#include <limits> 
#include <string>
#include <vector>
#include <numeric>
#include <iostream> 
#include <algorithm>
#include <unordered_map>
#include <memory>
#include <variant>
#include <optional>
#include <tuple>
#include <format>

enum class OrderType {
    
    GoodTillCancel,
    FillAndKill,
    FillOrKill,
    GoodForDay,
    Market,
};

enum class Side {
    Buy,
    Sell
};

using Price = std::int32_t; //
using Quantity = std::uint32_t;
using OrderId = std::uint64_t;

struct LevelInfo {
    // aggregated info at a price level, 
    // will be used in public APIs to get the state of the orderbooks.
    Price price;
    Quantity quantity;
    std::uint32_t orderCount;
};

using LevelInfoList = std::vector<LevelInfo>;

class OrderBookLevelInfoList {
    public:
        OrderBookLevelInfoList(const LevelInfoList& bids, const LevelInfoList& asks)
            : bids_(bids)
            , asks_(asks) 
        {   }
        
        const LevelInfoList& getBids() const { return bids_; }
        const LevelInfoList& getAsks() const { return asks_; }

    private:
        LevelInfoList bids_;
        LevelInfoList asks_;
};

class Order {
    public:
        Order(OrderType orderType, OrderId orderId, Side side, Price price, Quantity quantity, std::time_t timestamp)
            : type_{orderType}
            , id_{orderId}
            , side_{side}
            , price_{price}
            , initialQuantity_{quantity}
            , remainingQuantity_{quantity}
            , timestamp_{timestamp} 

        {   }

        Order(OrderId orderId, Side side, Price price, Quantity quantity)
            : Order{OrderType::Market, orderId, side, Constants::InvalidPrice, quantity, std::time_t(nullptr)} 
        {   }

        OrderType getOrderType() const { return type_; }
        OrderId getOrderId() const { return id_; }
        Side getSide() const { return side_; }
        Price getPrice() const { return price_; }
        Quantity getInitialQuantity() const { return initialQuantity_; }
        Quantity getRemainingQuantity() const { return remainingQuantity_; }
        Quantity getFilledQuantity() const { return initialQuantity_ - remainingQuantity_; }
        bool isFilled() const { return getRemainingQuantity() == 0; }
        std::time_t getTimestamp() const { return timestamp_; }

        // when a trade happens, lowest quantity associated between both orders as a quantity to fill both orders
        void fillQuantity(Quantity quantity) {
            if (quantity > getRemainingQuantity()) {
                // std::format is a cpp20 quirk
                throw std::logic_error(std::format("Order ({}) quantity exceeds remaining quantity", getOrderId()));
            }
            remainingQuantity_ -= quantity;
        }
        
    private:
        OrderType type_;
        OrderId id_;
        Side side_;
        Price price_;
        Quantity initialQuantity_;
        Quantity remainingQuantity_;
        std::time_t timestamp_;
};

// single order will be in multiple data structures, so we want a reference semantic
using OrderPointer = std::shared_ptr<Order>;  // stored in both orders dictionary and bid and ask based dicts
using OrderPointers = std::list<OrderPointer>; // for simplicity, lists over vectors

// abstraction: a lightweight represenation to an order is going to modify
// common APIs, add, modify. for add you need order, modify: can be cancel requires order id, or replace (price, quantity)


class OrderModify {
    public:

        OrderModify(OrderId orderId_, Side side, Price price, Quantity quantity, std::time_t timestamp)
            : orderId_{orderId_}
            , side_{side}   
            , price_{price}
            , quantity_{quantity}
            , timestamp_{timestamp} 
        {   }


        OrderId getOrderId() const { return orderId_; }
        Price getPrice() const { return price_; }
        Side getSide() const { return side_; }
        Quantity getQuantity() const { return quantity_; }
        std::time_t getTimestamp() const { return timestamp_; }

        OrderPointer toOrderPointer(OrderType orderType) const {
            // converting a given order that already exists, transforming it with this modify order, into a new order
            return std::make_shared<Order>(orderType, orderId_, side_, price_, quantity_, timestamp_);
        }

    private:
        OrderId orderId_;
        Price price_;
        Side side_;
        Quantity quantity_;
        std::time_t timestamp_;
};

// trade object is aggreation of two trade objects, bid trade info object, ask trade info object
struct TradeInfo {
    OrderId orderId_;
    Price price_;
    Quantity quantity_;
    std::time_t timestamp;
};

class Trade 
{
public:
    Trade(const TradeInfo& bidTrade, const TradeInfo& askTrade)
        : bidTrade_{bidTrade}
        , askTrade_{askTrade}

    {   }

    const TradeInfo& getBidTrade() const { return bidTrade_; }
    const TradeInfo& getAskTrade() const { return askTrade_; }

private:
    TradeInfo bidTrade_;
    TradeInfo askTrade_;
};

// one order may sweep a bunch of orders 
using Trades = std::vector<Trade>;

class Orderbook {

public:
    Orderbook() = default;

    // add order, modify order (cancel, replace), get level info, get trades
    void addOrder(const OrderPointer& orderPtr) {
        // implementation
    }

    void modifyOrder(const OrderModify& orderModify) {
        // implementation
    }

    OrderBookLevelInfoList getLevelInfo(std::size_t depth) const {
        // implementation
        return OrderBookLevelInfoList({}, {});
    }

    Trades getTrades() const {
        // implementation
        return Trades{};
    }

private:

    struct OrderEntry {
        OrderPointer order_ {nullptr};
        OrderPointers::iterator location_; 

    };

    // maps, to represent bids and asks
    // bids are sorted in descending order from best bid, asks in ascending order from best ask
    std::map<Price, OrderPointers, std::greater<Price>> bids_;
    std::map<Price, OrderPointers, std::less<Price>> asks_;
    std::unordered_map<OrderId, OrderEntry> orders_; // all active orders

    // match and canMatch methods, private
    // think of a FillAndKill order method, if its can't match, it's never added to the orderbook (discarded)
    // if non-FillAndKill order, first added to the orderbook, than "match" the orderbook, resolves 
    // at the end of the match it needs to be removed

    bool canMatch(Side side, Price price) const {
        if (side == Side::Buy) {
            // buy order can match if there are asks at or below the price
            if (asks_.empty()) {
                return false;
            }
            const auto& [bestAsk, _] = *asks_.begin();
            return price >= bestAsk;
        } else {
            // sell order can match if there are bids at or above the price
            if (bids_.empty()) {
                return false;
            }
            const auto& [bestBid, _] = *bids_.begin();
            return price <= bestBid;
    }

    Trades MatchOrders() {
        Trades trades;
        trades.reserve(orders_.size()); // reserve max possible size
        // implementation of matching logic

        while (true) {
            if (bids_.empty() || asks_.empty()) {
                break; 
            }
            auto& [bidPrice, bids] = *bids_.begin();
            auto& [askPrice, asks] = *asks_.begin();

            if (bidPrice < askPrice) {
                break; 
            }
            while (bids.size() && asks.size())
            {
                auto& bid = bids.front();
                auto& ask = asks.front();

                Quantity quantity = std:min(bid->getRemainingQuantity(), ask->getRemainingQuantity());

                bid->Fill(quantity);
                ask->Fill(quantity);

                if (bid->isFilled()) {
                    bids.pop_front();
                    orders_.erase(bid->getOrderId());
                }

                if (ask->isFilled()) {
                    asks.pop_front();
                    orders_.erase(ask->getOrderId());
                }  

                if (bids.empty())
                {
                    bids_.erase(bidPrice);
                }

                if (asks.empty())
                {
                    asks_.erase(askPrice);
                }  
                trades.push_back(Trade(
                    TradeInfo{bid->getOrderId(), askPrice, quantity, std::time(nullptr)},
                    TradeInfo{ask->getOrderId(), askPrice, quantity, std::time(nullptr)}
                )); 
            }
            
        
        if (!bids_.empty()) {
            auto& [_, bids] = *bids_.begin();
            auto& order = bids.front();
            if (order->getOrderType() == OrderType::FillAndKill) {
                CancelOrder(order->getOrderId());
            }
        }

        if (!asks_.empty()) {
            auto& [_, asks] = *asks_.begin();
            auto& order = asks.front();
            if (order->getOrderType() == OrderType::FillAndKill) {
                CancelOrder(order->getOrderId());
            }
        }
        return trades;
    }

public:

    Trades AddOrder(OrderPointer order) {
        if (orders_.contains(order->getOrderId())) {
            return {}
        }

        if (order->GetOrderType() != OrderType::FillAndKill && !CanMatch(order->getSide(), order->getPrice())) {
            // add to orderbook
            OrderPointers::iterator iterator;

            if (order->getSide() == Side::Buy) {
                auto& orders = bids_[order->getPrice()];
                orders.push_back(order);
                iterator = std::next(orders.begin(), orders.size() - 1);
            } else {
                auto& orders = asks_[order->getPrice()];
                orders.push_back(order);
                iterator = std::next(orders.begin(), orders.size() - 1);
            }

            orders_.insert({order->getOrderId(), OrderEntry{order, iterator}});
            return MatchOrders();
        }

        void CancelOrder(OrderId orderId) {
            return;
            }
            const auto& [order, iterator] = orders_.at(orderId);
            orders_.erase(orderId);

            if (order->GetSide() == Side::Sell) {
                auto price = order->GetPrice();
                auto& orders = asks_.at(price);
                orders.erase(iterator);
                if (orders.empty()) {
                    asks_.erase(price);
                }
            }
            else {
                auto price = order->GetPrice();
                auto& orders = bids_.at(price);
                orders.erase(iterator);
                if (orders.empty()) {
                    bids_.erase(price);
                }
            }
            Trades MatchOrders(OrderModify order) {
                if (!orders_.contains(order.getOrderId())) {
                    return {};
                }
                const auto& [existingOrder, iterator] = orders_.at(order.getOrderId());
                CancelOrder(order.getOrderId());
                return AddOrder(order.ToOrderPointer(existingOrder->getOrderType()));

            }

            std::size_t Size() const {
                return orders_.size();

            OrderBookLevelInfoList GetOrderInfo() const
            {
                LevelInfoList bidInfos;
                LevelInfoList askInfos;

                auto CreateLevelInfo = [](Price price, const OrderPointers& orders) {
                    [](std::size_t runningSum, const OrderPointer& order) {
                        return runningSum + orderPtr->getRemainingQuantity();
                    };

                for (const auto& [price, orders] : bids_) {
                    bidInfos.push_back(CreateLevelInfos(price, orders));

                for (const auto& [price, orders] : asks_) {
                    askInfos.push_back(CreateLevelInfos(price, orders));
                
                return OrderBookLevelInfoList(bidInfos, askInfos); 
    }
};

int main() {

    return 0;
}


