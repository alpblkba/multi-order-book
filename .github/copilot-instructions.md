# Copilot Instructions for multi-order-book

## Project Overview
A C++ order book implementation for managing buy/sell orders at different price levels. Early-stage project focused on core order matching and state management.

## Architecture & Data Structures

### Core Types (orderbook.cpp)
- **OrderType**: `GoodTillCancel` (persistent) vs `FillAndKill` (execute or cancel immediately)
- **Side**: Buy or Sell
- **Price**: `std::int32_t` - price levels
- **Quantity**: `std::uint32_t` - order quantity
- **OrderId**: `std::uint64_t` - unique order identifier

### Key Structs
- **Order**: Single order instance with id, side, price, quantity, type, timestamp
- **LevelInfo**: Aggregated data at a price level (price, total quantity, order count) - used in public APIs

### Expected Data Flow
1. Orders are added with OrderId, Side, Price, Quantity, and OrderType
2. Orders aggregate into price levels for efficient querying
3. GoodTillCancel orders persist until filled; FillAndKill orders execute immediately or cancel
4. LevelInfo provides read-only snapshots of each price level's state

## Implementation Patterns

### Data Structure Choices
- Use `std::map<Price, LevelInfo>` for buy side and `std::map<Price, LevelInfo>` for sell side (automatic price ordering)
- Use `std::unordered_map<OrderId, Order>` for order lookup by id
- Use `std::list<Order>` or similar within each level to maintain insertion order for time priority

### Matching Logic
- Priority: Price (better first) then Time (earlier first)
- Buy orders: sort prices descending (highest first)
- Sell orders: sort prices ascending (lowest first)
- Match orders when spread closes (buy_price >= sell_price)

## Development Guidelines

### C++ Style
- Use modern C++ features already imported: `std::optional`, `std::variant`, `std::tuple`, `std::format`
- Header-only implementation (no separate .h files yet) - keep in `orderbook.cpp`
- Use `std::int32_t`, `std::uint32_t`, `std::uint64_t` for explicit sizing

### Testing Strategy
Currently no test infrastructure. Consider adding:
- Unit tests for order matching (same price, different prices, cancellations)
- Benchmark for large order counts (performance target: sub-millisecond operations)

### Common Tasks
- Adding order: insert into both OrderId map and price level
- Canceling order: remove from OrderId map and decrement level quantity/count
- Querying level: return aggregated LevelInfo without exposing internal orders
- Matching: iterate matching buy/sell pairs, update quantities, remove filled orders

## Known Limitations & TODOs
- Main function is empty - integration point TBD
- No partial fill handling yet
- No public API layer (will likely need getter methods for level data)
- Performance untested with large order counts
