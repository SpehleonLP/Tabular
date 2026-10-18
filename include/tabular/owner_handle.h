#ifndef TABULAR_OWNER_HANDLE_H
#define TABULAR_OWNER_HANDLE_H

#include <cstdint>

namespace tabular
{

class relation_store;

// A contributor's lifetime made a scope: dropping the handle retracts every row
// the owner holds, so a contributor that dies without cleaning up (a crashed
// package, a thrown reload) leaves nothing behind. Move-only; a moved-from
// handle holds nothing and retracts nothing. The store must outlive the handle
// and must not be moved while one is held: relation_store stays movable (a test
// pins it), so the handle cannot enforce this, and a handle left pointing at a
// moved-from store retracts through a hollow object.
//
// One owner, one handle. Two live handles for the same owner each retract the
// other's rows when they end: the first to go takes everything the owner holds,
// including rows loaded through the other. Hold the owner's handle in one place
// and move it, never copy the owner id into a second handle.
//
// It retracts from EVERY table, read-only tables and grids included, which is
// relation_store::Retract and nothing more. Definition.md says "RAII handles
// retract them" of rows each owned by a contributor, with no exception for how
// the rows arrived, and the handle exists for crash recovery: a contributor's
// loaded files outliving it are rows nobody can reload or remove, because the
// only thing that knew to retract them is gone. Retract is also never refused,
// so a destructor can always finish it. A reload that must keep a read-only
// table's rows across a handle's life retracts nothing and loads again
// instead; the handle is for the end of a contributor, not for reloading one.
class owner_handle
{
public:
	owner_handle(relation_store& store, uint64_t owner);
	~owner_handle();

	owner_handle(owner_handle&& other) noexcept;
	// Retracts the owner this handle held, then takes other's.
	owner_handle& operator=(owner_handle&& other) noexcept;
	owner_handle(owner_handle const&)            = delete;
	owner_handle& operator=(owner_handle const&) = delete;

	uint64_t owner() const { return _owner; }
	bool     holds() const { return _store != nullptr; }

	// Retracts now. Later calls, and the destructor, do nothing.
	void Release();

private:
	relation_store* _store = nullptr;
	uint64_t        _owner = 0;
};

}

#endif // TABULAR_OWNER_HANDLE_H
