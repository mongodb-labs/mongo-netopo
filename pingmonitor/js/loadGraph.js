	function loadData(){
	
		var html = "";
		
		var width = 670,
			height = 500,
			fill = d3.scale.category20c(),
			padding = 6;
			
		var m = 8;
		var x = d3.scale.ordinal().domain(d3.range(m)).rangePoints([0, width], 1);
	
		var bigRadius = 20;
			littleRadius = 10;
			
		var nodes = [];
		var links = [];
		
		var servname, portnum, dbname, collpref;
		
		if( document.getElementById("servname").value == "" ){
			alert("Please enter a server name!");
			return;
		}
		else
			servname = document.getElementById("servname").value;
			
		if( document.getElementById("portnum").value == "" ){
			alert("Please enter a port number!");
			return;
		}
		else
			portnum = document.getElementById("portnum").value;
			
		if( document.getElementById("dbname").value == "" ){
			alert("Please enter a database name!");
			return;
		}
		else
			dbname = document.getElementById("dbname").value;
			
		if( document.getElementById("collpref").value == "" ){
			alert("Please enter a collection prefix!");
			return;
		}
		else
			collpref = document.getElementById("collpref").value;

		
		var graphsurl = "http://" + servname + ":" + portnum + "/" + dbname + "/pingMonitor." + collpref + ".graphs/";
		
		$.ajax({
		  url: graphsurl,
		  type: 'get',
		  dataType: 'jsonp',
		  jsonp: 'jsonp', // mongod is expecting the parameter name to be called "jsonp"
		  success: function (data) {
		  	
			document.getElementById("chart").innerHTML="";	  	
		  	
		  	/**************************************** GET NODE DATA ****************************************/
		  
			for( var n in data.rows[1].nodes ){
				
				newNode = {};
				
				newNode[ "hostName" ] = data.rows[1].nodes[n].hostName;
				var role = data.rows[1].nodes[n].type.role;
				newNode[ "role" ] = role;

				newNode[ "toShow" ] = {};
				newNode[ "toShow" ][ "hostname" ] = data.rows[1].nodes[n].hostName;
				
				if( data.rows[1].nodes[n].shardName )
					newNode[ "toShow" ][ "shardName" ] = data.rows[1].nodes[n].shardName;
					
				newNode[ "toShow" ][ "role" ] = data.rows[1].nodes[n].type.role;
				newNode[ "toShow" ][ "os" ] = data.rows[1].nodes[n].hostInfo.os.name;
				newNode[ "toShow" ][ "mongodbVersion" ] = data.rows[1].nodes[n].serverStatus.version;
				newNode[ "toShow" ][ "connections" ] = data.rows[1].nodes[n].serverStatus.connections.current;
				newNode[ "toShow" ][ "uptimeSecs" ] = data.rows[1].nodes[n].serverStatus.uptime;
				
				if( role == "primary" || role == "secondary" ){
					newNode[ "group" ] = data.rows[1].nodes[n].shardName;
				}
				else{
					newNode[ "group" ] = newNode[ "hostName" ];
				}
				
				if( role == "secondary" )
					newNode[ "radius" ] = littleRadius;
				else
					newNode[ "radius" ] = bigRadius;
						
				if( role == "config" || role == "mongos" ){
					newNode[ "color" ] = d3.rgb( fill( newNode["role"] ) );
					newNode[ "strokecolor" ] = d3.rgb( fill( newNode["role"] ) ).darker(3);
				}
				else{
					newNode[ "color" ] = d3.rgb( fill( newNode["group"] ) );
					newNode[ "strokecolor" ] = d3.rgb( fill( newNode["group"] ) ).brighter(3);
				}
					
				nodes.push( newNode );
				
			}
			
			var allGroups = {};
			for( var i=0; i<nodes.length; i++){
				var myGroup = nodes[i].group;
				if( allGroups[ myGroup ] == null ){
					allGroups[ myGroup ] = {};
				//	var g = parseInt( Math.random() * m );
					if( i % 2 == 0 )
						allGroups[ myGroup ]["plusx"] = 2*i;
					else
						allGroups[ myGroup ]["plusx"] = -2*i;
				}
			}
			
		/************************************* GET EDGE DATA ****************************************/
		
			for( var eo in data.rows[1].edges ){
				for( var ei in data.rows[1].edges[eo] ){
					if( nodes[ parseInt(eo) ][ parseInt(ei) ] == null && nodes[ parseInt(ei) ].role != "secondary" && nodes[ parseInt(eo) ].role != "secondary" ){
						var newEdge = {};
						newEdge[ "source" ] = parseInt(eo);
						newEdge[ "target" ] = parseInt(ei);
						newEdge[ "src1" ] = {}
						newEdge[ "src1" ][ "source" ] = data.rows[1].nodes[eo].hostName;
						newEdge[ "src1" ][ "target" ] = data.rows[1].nodes[ei].hostName;
						newEdge[ "src1" ][ "pingTimeMicros" ] = data.rows[1].edges[eo][ei].pingTimeMicros;
						newEdge[ "src1" ][ "isConnected" ] = data.rows[1].edges[eo][ei].isConnected;
						newEdge[ "src1" ][ "bytesSent" ] = data.rows[1].edges[eo][ei].bytesSent;
						newEdge[ "src1"  ][ "bytesRecd" ] = data.rows[1].edges[eo][ei].bytesRecd;
						newEdge[ "src2" ] = {}
						newEdge[ "src2" ][ "source" ] = data.rows[1].nodes[ei].hostName;
						newEdge[ "src2" ][ "target" ] = data.rows[1].nodes[eo].hostName;
						newEdge[ "src2" ][ "pingTimeMicros" ] = data.rows[1].edges[ei][eo].pingTimeMicros;
						newEdge[ "src2" ][ "isConnected" ] = data.rows[1].edges[ei][eo].isConnected;
						newEdge[ "src2" ][ "bytesSent" ] = data.rows[1].edges[ei][eo].bytesSent;
						newEdge[ "src2" ][ "bytesRecd" ] = data.rows[1].edges[ei][eo].bytesRecd;
						links.push( newEdge );
					}
				}
			}

		/******************************************** BUILD FORCE GRAPH **************************************************/
		
			var groups = d3.nest()
				.key(function(d)
					{ return d.group; }
				).entries(nodes);

			var groupPath = function(d) {
				return "M" + 
				  d3.geom.hull(d.values.map(function(i) { return [i.x, i.y]; }))
					.join("L")
				+ "Z";
			};

			var groupFill = function(d, i) { return "white";};

			var vis = d3.select("#chart").append("svg")
				.attr("width", w )
				.attr("height", h);

			var force = d3.layout.force()
				.nodes(nodes)
				.links(links)
				.linkDistance(250)
				.linkStrength(0.5)
				.friction(0.2)
				.size([width, height])
				.start();
				
			var link = vis.selectAll(".link")
			  .data(links)
			.enter().append("line")
			  .attr("class", "link");

			var node = vis.selectAll("circle.node")
				.data(nodes)
			  .enter().append("circle")
				.attr("class", "node")
				.attr("cx", function(d) { return d.cx; })
				.attr("cy", function(d) { return d.cy; })
				.attr("r", function(d) { return d.radius; })
				.style("fill", function(d) { return d.color; } )
				.style("stroke", function(d) { return d.strokecolor; })
				.style("stroke-width", 2)
				.call(force.drag);

			vis.style("opacity", 1e-6)
			  .transition()
				.duration(1000)
				.style("opacity", 1);

			force.on("tick", function(e) {

			// Push different nodes in different directions for clustering.
			nodes.forEach(function(o, i) {
			  	o.x += (allGroups[ o.group ][ "plusx" ] * e.alpha);
			  	o.y += (Math.random(5) * e.alpha);
		  	}); 
	
			  node
			  .each(cluster(60 * e.alpha * e.alpha))
			//  .each(collide(.5))
			  .attr("cx", function(d) { return d.x; })
			  .attr("cy", function(d) { return d.y; });

			  link.attr("x1", function(d) { return d.source.x; })
				  .attr("y1", function(d) { return d.source.y; })
				  .attr("x2", function(d) { return d.target.x; })
				  .attr("y2", function(d) { return d.target.y; });

			  vis.selectAll("path")
				.data(groups)
				  .attr("d", groupPath)
				.enter().insert("path", "circle")
				  .style("fill", groupFill)
				  .style("stroke", groupFill)
				  .style("stroke-width", 60)
				  .style("stroke-linejoin", "round")
				  .style("opacity", .2) 
				  .attr("d", groupPath);			  
			});

		// Move d to be adjacent to the cluster node.
		function cluster(alpha) {
		  var max = {};

		  // Find the largest node for each cluster.
		  nodes.forEach(function(d) {
			if (!(d.color in max) || (d.radius > max[d.color].radius)) {
			  max[d.color] = d;
			}
		  });

		  return function(d) {
			var node = max[d.color],
				l,r,x,y,
				k = 1,
				i = -1;

			// For cluster nodes, apply custom gravity.
			if (node == d) {
			  node = {x: w / 2, y: h / 2, radius: -d.radius};
			  k =.1 * Math.sqrt(d.radius);
			}

			x = d.x - node.x;
			y = d.y - node.y;
			l = Math.sqrt(x * x + y * y);
			r = d.radius + node.radius;
			if (l != r) {
			  l = (l - r) / l * alpha * k;
			  d.x -= x *= l;
			  d.y -= y *= l;
			  node.x += x;
			  node.y += y;
			}
		  };
		}

		d3.select("svg").on("dblclick", function() {
		  nodes.forEach(function(o, i) {
			o.x += (Math.random() - .5) * 40;
			o.y += (Math.random() - .5) * 40;
		  });
		  force.resume();
		});

		/*************************************** SHOW DATA ON HOVER *****************************************/
		
		$('svg circle').tipsy({ 
			gravity: 'w', 
			html: true, 
			title: function() {
				var out = "<table>";
				for( var n in (this.__data__.toShow) ){
					out += "<tr>";
					out += "<td align='left'><b>" + n + ":<b></td>";
					out += "<td>" + this.__data__.toShow[n] + "</td>";
					out += "</tr>";				
				}
				out += "</table>";
				return out;
			}
		  });
		  
		$('svg line').hover(function(){
			$(".edgeInfo").css("border-color","yellow");
			$(this).css("stroke" , "yellow");
			
			
			var out1 = "Direction 1: <br/>";
			out1 += "<table>";
			for( var n in this.__data__["src1"]){
					out1 += "<tr>";
					out1 += "<td align='left'><b>" + n + ":<b></td>";
					out1 += "<td>" + this.__data__["src1"][n] + "</td>";
					out1 += "</tr>";	
			}
			out1 += "</table>";
			
			$("#e1").html(out1);
			
			var out2 = "Direction 2: <br/>";
			out2 += "<table>";
			for( var n in this.__data__["src2"]){
					out2 += "<tr>";
					out2 += "<td align='left'><b>" + n + ":<b></td>";
					out2 += "<td>" + this.__data__["src2"][n] + "</td>";
					out2 += "</tr>";	
			}
			out2 += "</table>";
			
			$("#e2").html(out2);
			
		},
		function(){
			$(".edgeInfo").css("border-color", "gray");
			$(this).css("stroke","gray");
			$("#e1").html("");
			$("#e2").html("");
		});
	  		  
			console.log('success' , data );
		  },
		  error: function (XMLHttpRequest, textStatus, errorThrown) {
		  	alert("Unable to send request. Please check input data.");
			console.log('error', errorThrown);
		  }
		});
  	}