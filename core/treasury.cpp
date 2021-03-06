// Copyright 2018 The Beam Team
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "treasury.h"
#include "proto.h"
#include "serialization_adapters.h"

namespace beam
{
	using namespace ECC;

	struct Treasury::Response::Group::Reader
		:public TxBase::IReader
	{
		const Group& m_This;
		size_t m_iOut;
		size_t m_iKrn;

		Reader(const Group& g) :m_This(g) {}

		virtual void Clone(Ptr& p) override
		{
			p.reset(new Reader(m_This));
		}

		virtual void Reset() override
		{
			m_iOut = 0;
			m_iKrn = 0;

			m_pUtxoIn = NULL;
			NextUtxoOut();
			NextKernel();
		}

		virtual void NextUtxoIn() override
		{
			assert(false);
		}

		virtual void NextUtxoOut() override
		{
			if (m_iOut < m_This.m_vCoins.size())
			{
				m_pUtxoOut = m_This.m_vCoins[m_iOut].m_pOutput.get();
				m_iOut++;
			}
			else
				m_pUtxoOut = NULL;
		}

		virtual void NextKernel() override
		{
			if (!m_iKrn)
			{
				m_pKernel = m_This.m_pKernel.get();
				m_iKrn++;
			}
			else
				m_pKernel = NULL;
		}
	};

	class Treasury::ThreadPool
	{
		std::vector<std::thread> m_vThreads;
	public:

		struct Context
		{
			virtual void Do(size_t iTask) = 0;

			void DoRange(size_t i0, size_t i1)
			{
				for (; i0 < i1; i0++)
					Do(i0);
			}

			void DoAll(size_t nTasks)
			{
				ThreadPool tp(*this, nTasks);
			}
		};

		struct Verifier
			:public Context
		{
			volatile bool m_bValid;
			Verifier() :m_bValid(true) {}

			virtual void Do(size_t iTask) override
			{
				typedef InnerProduct::BatchContextEx<100> MyBatch;

				std::unique_ptr<MyBatch> p(new MyBatch);
				p->m_bEnableBatch = true;
				MyBatch::Scope scope(*p);

				if (!Verify(iTask) || !p->Flush())
					m_bValid = false; // sync isn't required
			}

			virtual bool Verify(size_t iTask) = 0;

		};

		ThreadPool(Context& ctx, size_t nTasks)
		{
			size_t numCores = std::thread::hardware_concurrency();
			if (!numCores)
				numCores = 1; //?
			if (numCores > nTasks)
				numCores = nTasks;

			m_vThreads.resize(numCores);
			size_t iTask0 = 0;

			for (size_t i = 0; i < m_vThreads.size(); i++)
			{
				size_t iTask1 = nTasks * (i + 1) / numCores;
				assert(iTask1 > iTask0); // otherwise it means that redundant threads were created

				m_vThreads[i] = std::thread(&Context::DoRange, &ctx, iTask0, iTask1);

				iTask0 = iTask1;
			}

			assert(iTask0 == nTasks);
		}

		~ThreadPool()
		{
			for (size_t i = 0; i < m_vThreads.size(); i++)
				if (m_vThreads[i].joinable())
					m_vThreads[i].join();
		}
	};

	void Treasury::Request::Group::AddSubsidy(AmountBig& res) const
	{
		for (size_t i = 0; i < m_vCoins.size(); i++)
			res += m_vCoins[i].m_Value;
	}

	void Treasury::Response::Group::Coin::get_SigMsg(Hash::Value& hv) const
	{
		Hash::Processor() << m_pOutput->m_Commitment >> hv;
	}

	void Treasury::get_ID(Key::IKdf& kdf, PeerID& pid, Scalar::Native& sk)
	{
		Key::ID kid;
		kid.m_Idx = 0;
		kid.m_Type = FOURCC_FROM(tRid);

		kdf.DeriveKey(sk, kid);
		proto::Sk2Pk(pid, sk);
	}

	void Treasury::Response::Group::Create(const Request::Group& g, Key::IKdf& kdf, uint64_t& nIndex)
	{
		m_vCoins.resize(g.m_vCoins.size());

		Scalar::Native sk, offset = Zero;

		for (size_t iC = 0; iC < m_vCoins.size(); iC++)
		{
			const Request::Group::Coin& c0 = g.m_vCoins[iC];
			Coin& c = m_vCoins[iC];

			c.m_pOutput.reset(new Output);
			c.m_pOutput->m_Incubation = c0.m_Incubation;

			Key::IDV kidv;
			ZeroObject(kidv);
			kidv.m_Idx = nIndex++;
			kidv.m_Type = FOURCC_FROM(Tres);
			kidv.m_Value = c0.m_Value;

			c.m_pOutput->Create(sk, kdf, kidv);
			offset += sk;

			Hash::Value hv;
			c.get_SigMsg(hv);
			c.m_Sig.Sign(hv, sk);
		}

		Key::ID kid;
		kid.m_Idx = nIndex++;
		kid.m_Type = FOURCC_FROM(KeR3);
		kdf.DeriveKey(sk, kid);

		m_pKernel.reset(new TxKernel);
		m_pKernel->Sign(sk);
		offset += sk;

		offset = -offset;
		m_Base.m_Offset = offset;

	}

	bool Treasury::Response::Group::IsValid(const Request::Group& g) const
	{
		if (m_vCoins.size() != g.m_vCoins.size())
			return false;

		Mode::Scope scope(Mode::Fast);

		if (!m_pKernel ||
			m_pKernel->m_Fee ||
			(m_pKernel->m_Height.m_Min > Rules::HeightGenesis) ||
			(m_pKernel->m_Height.m_Max != MaxHeight))
			return false;

		TxBase::Context ctx;
		ctx.m_bVerifyOrder = false;
		if (!ctx.ValidateAndSummarize(m_Base, Reader(*this)))
			return false;

		Point::Native comm, comm2;

		for (size_t iC = 0; iC < m_vCoins.size(); iC++)
		{
			const Request::Group::Coin& c0 = g.m_vCoins[iC];
			const Coin& c = m_vCoins[iC];

			if (!c.m_pOutput ||
				c.m_pOutput->m_pPublic ||
				c.m_pOutput->m_Coinbase ||
				(c.m_pOutput->m_Incubation != c0.m_Incubation) ||
				!comm.Import(c.m_pOutput->m_Commitment))
				return false;

			// verify the value
			comm2 = Context::get().H * c0.m_Value;
			comm2 = -comm2;

			ctx.m_Sigma += comm2;
			comm += comm2;

			Hash::Value hv;
			c.get_SigMsg(hv);
			if (!c.m_Sig.IsValid(hv, comm))
				return false;
		}

		return (ctx.m_Sigma == Zero);
	}

	void Treasury::Response::HashOutputs(Hash::Value& hv) const
	{
		Hash::Processor hp;
		for (size_t iG = 0; iG < m_vGroups.size(); iG++)
		{
			const Group& g = m_vGroups[iG];
			for (size_t iC = 0; iC < g.m_vCoins.size(); iC++)
				hp << g.m_vCoins[iC].m_pOutput->m_Commitment;
		}
		hp >> hv;
	}

	bool Treasury::Response::Create(const Request& r, Key::IKdf& kdf, uint64_t& nIndex)
	{
		PeerID pid;
		Scalar::Native sk;
		get_ID(kdf, pid, sk);

		if (pid != r.m_WalletID)
			return false;

		m_WalletID = r.m_WalletID;

		m_vGroups.resize(r.m_vGroups.size());

		struct Context
			:public ThreadPool::Context
		{
			const Request& m_Req;
			Response& m_Resp;
			Key::IKdf& m_Kdf;
			uint64_t m_Index0;

			Context(const Request& req, Response& resp, Key::IKdf& kdf, uint64_t nIndex)
				:m_Req(req)
				,m_Resp(resp)
				,m_Kdf(kdf)
				,m_Index0(nIndex)
			{}

			uint64_t get_IndexAt(size_t iG) const
			{
				uint64_t nIndex = m_Index0;
				for (size_t i = 0; i < iG; i++)
					nIndex += m_Req.m_vGroups[i].m_vCoins.size() + 1;

				return nIndex;
			}

			virtual void Do(size_t iTask) override
			{
				uint64_t nIndex = get_IndexAt(iTask);
				m_Resp.m_vGroups[iTask].Create(m_Req.m_vGroups[iTask], m_Kdf, nIndex);
				assert(get_IndexAt(iTask + 1) == nIndex);
			}

		} ctx(r, *this, kdf, nIndex);

		ctx.DoAll(m_vGroups.size());
		nIndex = ctx.get_IndexAt(m_vGroups.size());

		Hash::Value hv;
		HashOutputs(hv);

		m_Sig.Sign(hv, sk);
		return true;
	}

	bool Treasury::Response::IsValid(const Request& r) const
	{
		if ((m_vGroups.size() != r.m_vGroups.size()) ||
			(m_WalletID != r.m_WalletID))
			return false;

		struct Context
			:public ThreadPool::Verifier
		{
			const Request& m_Req;
			const Response& m_Resp;

			Context(const Request& req, const Response& resp)
				:m_Req(req)
				,m_Resp(resp)
			{}

			virtual bool Verify(size_t iTask) override
			{
				return m_Resp.m_vGroups[iTask].IsValid(m_Req.m_vGroups[iTask]);
			}

		} ctx(r, *this);

		ctx.DoAll(m_vGroups.size());
		if (!ctx.m_bValid)
			return false;

		// finally verify the signature
		Point::Native pk;
		if (!proto::ImportPeerID(pk, r.m_WalletID))
			return false;

		Hash::Value hv;
		HashOutputs(hv);

		return m_Sig.IsValid(hv, pk);
	}

	Treasury::Entry* Treasury::CreatePlan(const PeerID& pid, Amount nPerBlockAvg, const Parameters& pars)
	{
		EntryMap::iterator it = m_Entries.find(pid);
		if (m_Entries.end() != it)
			m_Entries.erase(it);

		Entry& e = m_Entries[pid];
		Request& r = e.m_Request;
		r.m_WalletID = pid;

		assert(pars.m_StepMin);
		nPerBlockAvg *= pars.m_StepMin;

		Height h0 = 0;
		for (Height h = 0; h < pars.m_MaxHeight; h += pars.m_StepMin)
		{
			if (r.m_vGroups.empty() || (h - h0 >= pars.m_MaxDiffPerBlock))
			{
				r.m_vGroups.emplace_back();
				h0 = h;
			}

			Request::Group::Coin& c = r.m_vGroups.back().m_vCoins.emplace_back();
			c.m_Incubation = h;
			c.m_Value = nPerBlockAvg;
		}

		return &e;
	}

	size_t Treasury::get_OverheadFor(const AmountBig& x)
	{
		Block::Body body;
		body.ZeroInit();
		body.m_Subsidy = x;

		return get_BlockSize(body);
	}

	size_t Treasury::get_BlockSize(const Block::Body& body)
	{
		SerializerSizeCounter ssc;
		ssc & body;
		return ssc.m_Counter.m_Value;
	}

	void Treasury::Build(std::vector<Block::Body>& res) const
	{
		// Assuming all the plans are generated with the same group/incubation parameters.
		for (size_t iG = 0; ; iG++)
		{
			bool bNoPeers = true, bNoBlock = true;

			Block::Body body;
			size_t nOverhead = 0, nSizeTotal = 0;

			for (EntryMap::const_iterator it = m_Entries.begin(); m_Entries.end() != it; )
			{
				const Entry& e = (it++)->second;
				if (!e.m_pResponse)
					continue;
				const Response& resp = *e.m_pResponse;

				if (iG >= resp.m_vGroups.size())
					continue;
				bNoPeers = false;

				if (bNoBlock)
				{
					body.ZeroInit();
					nOverhead = get_OverheadFor(body.m_Subsidy); // the BodyBase size slightly depends on its subsidy. Hence it should be recalculated after adding every peer
					nSizeTotal = nOverhead;
				}

				const Response::Group& g = resp.m_vGroups[iG];
				Response::Group::Reader r(g);
				size_t nSizeNetto = r.get_SizeNetto();

				AmountBig subsNext = body.m_Subsidy;
				e.m_Request.m_vGroups[iG].AddSubsidy(subsNext);

				size_t nOverheadNext = get_OverheadFor(subsNext);

				size_t nSizeAfterMerge = nSizeTotal + nSizeNetto + nOverheadNext - nOverhead;
				if (nSizeAfterMerge <= Rules::get().MaxBodySize)
				{
					// merge
					TxVectors::Writer(body, body).Dump(std::move(r));

					Scalar::Native off = body.m_Offset;
					off += g.m_Base.m_Offset;
					body.m_Offset = off;

					bNoBlock = false;

					body.m_Subsidy = subsNext;
					nSizeTotal = nSizeAfterMerge;
					nOverhead = nOverheadNext;

					assert(get_BlockSize(body) == nSizeTotal);
				}
				else
				{
					if (bNoBlock)
						throw std::runtime_error("treasury group too large");

					res.push_back(std::move(body));
					bNoBlock = true;
					it--; // retry
				}
			}

			if (bNoPeers)
				break;

			if (!bNoBlock)
				res.push_back(std::move(body));
		}

		// finalize
		struct Context
			:public ThreadPool::Verifier
		{
			std::vector<Block::Body>& m_Res;

			Context(std::vector<Block::Body>& res)
				:m_Res(res)
			{}

			virtual bool Verify(size_t iTask) override
			{
				m_Res[iTask].Normalize();

				return m_Res[iTask].IsValid(HeightRange(Rules::HeightGenesis + iTask), true);
			}

		} ctx(res);

		ctx.DoAll(res.size());
		if (!ctx.m_bValid)
			throw std::runtime_error("Invalid block generated");
	}

} // namespace beam
