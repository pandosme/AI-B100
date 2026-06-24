class PolygonSelector {
	constructor(canvasId, options) {
		this.canvas = document.getElementById(canvasId);
		this.ctx = this.canvas.getContext('2d');
		this.points = [];
		this.maxPoints = options.maxPoints || 8;
		this.minPoints = options.minPoints || 3;
		this.onchange = options.onchange || null;
		this.enabled = options.enabled !== false;
		this.dragIndex = -1;
		this.hoverIndex = -1;
		this.handleRadius = 10;
		this.canvas.addEventListener('mousedown', this._onMouseDown.bind(this));
		this.canvas.addEventListener('mousemove', this._onMouseMove.bind(this));
		this.canvas.addEventListener('mouseup', this._onMouseUp.bind(this));
		this.canvas.addEventListener('mouseleave', this._onMouseUp.bind(this));
		this.canvas.addEventListener('dblclick', this._onDoubleClick.bind(this));
	}

	setEnabled(enabled) {
		this.enabled = !!enabled;
		this.redraw();
	}

	setPoints(points) {
		this.points = (points || []).slice(0, this.maxPoints).map(point => ({
			x: this._clamp(point.x),
			y: this._clamp(point.y)
		}));
		if (this.points.length < this.minPoints) {
			this.points = [
				{ x: 0, y: 0 },
				{ x: 1000, y: 0 },
				{ x: 1000, y: 1000 },
				{ x: 0, y: 1000 }
			];
		}
		this.redraw();
	}

	getPoints() {
		return this.points.map(point => ({
			x: Math.round(point.x),
			y: Math.round(point.y)
		}));
	}

	redraw() {
		this.ctx.clearRect(0, 0, this.canvas.width, this.canvas.height);
		if (!this.points.length) return;

		this.ctx.save();
		this.ctx.beginPath();
		this.ctx.moveTo(this.points[0].x, this.points[0].y);
		for (let index = 1; index < this.points.length; index++) {
			this.ctx.lineTo(this.points[index].x, this.points[index].y);
		}
		this.ctx.closePath();
		this.ctx.fillStyle = this.enabled ? 'rgba(0, 160, 80, 0.22)' : 'rgba(120, 120, 120, 0.18)';
		this.ctx.strokeStyle = this.enabled ? '#00a050' : '#777';
		this.ctx.lineWidth = 4;
		this.ctx.setLineDash(this.enabled ? [] : [14, 10]);
		this.ctx.fill();
		this.ctx.stroke();
		this.ctx.setLineDash([]);

		for (let index = 0; index < this.points.length; index++) {
			const point = this.points[index];
			this.ctx.beginPath();
			this.ctx.arc(point.x, point.y, this.handleRadius, 0, Math.PI * 2);
			this.ctx.fillStyle = index === this.hoverIndex ? '#ffcf33' : '#ffffff';
			this.ctx.strokeStyle = this.enabled ? '#00a050' : '#777';
			this.ctx.lineWidth = 3;
			this.ctx.fill();
			this.ctx.stroke();
		}
		this.ctx.restore();
	}

	_notifyChange() {
		if (this.onchange) this.onchange(this.getPoints());
	}

	_onMouseDown(event) {
		const mouse = this._mouseToCanvas(event);
		const handle = this._nearestHandle(mouse);
		if (handle >= 0) {
			this.dragIndex = handle;
			return;
		}

		if (this.points.length < this.maxPoints) {
			const edgeIndex = this._nearestEdge(mouse);
			if (edgeIndex >= 0) {
				this.points.splice(edgeIndex + 1, 0, mouse);
				this.dragIndex = edgeIndex + 1;
				this.redraw();
				this._notifyChange();
			}
		}
	}

	_onMouseMove(event) {
		const mouse = this._mouseToCanvas(event);
		if (this.dragIndex >= 0) {
			this.points[this.dragIndex] = mouse;
			this.redraw();
			this._notifyChange();
			return;
		}

		this.hoverIndex = this._nearestHandle(mouse);
		this.canvas.style.cursor = this.hoverIndex >= 0 ? 'move' : 'crosshair';
		this.redraw();
	}

	_onMouseUp() {
		this.dragIndex = -1;
	}

	_onDoubleClick(event) {
		const mouse = this._mouseToCanvas(event);
		const handle = this._nearestHandle(mouse);
		if (handle >= 0 && this.points.length > this.minPoints) {
			this.points.splice(handle, 1);
			this.hoverIndex = -1;
			this.redraw();
			this._notifyChange();
		}
	}

	_nearestHandle(mouse) {
		for (let index = 0; index < this.points.length; index++) {
			const point = this.points[index];
			const distance = Math.hypot(point.x - mouse.x, point.y - mouse.y);
			if (distance <= this.handleRadius * 2) return index;
		}
		return -1;
	}

	_nearestEdge(mouse) {
		let nearest = -1;
		let nearestDistance = 28;
		for (let index = 0; index < this.points.length; index++) {
			const next = (index + 1) % this.points.length;
			const distance = this._pointToSegmentDistance(mouse, this.points[index], this.points[next]);
			if (distance < nearestDistance) {
				nearest = index;
				nearestDistance = distance;
			}
		}
		return nearest;
	}

	_pointToSegmentDistance(point, start, end) {
		const dx = end.x - start.x;
		const dy = end.y - start.y;
		if (dx === 0 && dy === 0) return Math.hypot(point.x - start.x, point.y - start.y);
		const t = Math.max(0, Math.min(1, ((point.x - start.x) * dx + (point.y - start.y) * dy) / (dx * dx + dy * dy)));
		const x = start.x + t * dx;
		const y = start.y + t * dy;
		return Math.hypot(point.x - x, point.y - y);
	}

	_mouseToCanvas(event) {
		const rect = this.canvas.getBoundingClientRect();
		return {
			x: this._clamp(Math.round((event.clientX - rect.left) * (this.canvas.width / rect.width))),
			y: this._clamp(Math.round((event.clientY - rect.top) * (this.canvas.height / rect.height)))
		};
	}

	_clamp(value) {
		const number = Number.isFinite(value) ? value : 0;
		return Math.max(0, Math.min(1000, number));
	}
}
