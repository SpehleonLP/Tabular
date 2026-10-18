#include "tabular/owner_handle.h"

#include "tabular/relation.h"

#include <utility>

namespace tabular
{

owner_handle::owner_handle(relation_store& store, uint64_t owner)
	: _store(&store)
	, _owner(owner)
{
}

owner_handle::~owner_handle()
{
	Release();
}

owner_handle::owner_handle(owner_handle&& other) noexcept
	: _store(std::exchange(other._store, nullptr))
	, _owner(other._owner)
{
}

owner_handle& owner_handle::operator=(owner_handle&& other) noexcept
{
	if (this != &other)
	{
		Release();
		_store = std::exchange(other._store, nullptr);
		_owner = other._owner;
	}
	return *this;
}

void owner_handle::Release()
{
	if (_store)
	{
		_store->Retract(retract_owner{_owner});
		_store = nullptr;
	}
}

}
